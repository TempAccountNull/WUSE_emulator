#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <utils/finally.hpp>

#include <array>
#include <bit>
#include <cstdlib>
#include <string>

namespace sogen::test
{
    namespace
    {
        bool set_environment(const char* name, const char* value)
        {
#ifdef OS_WINDOWS
            return _putenv_s(name, value) == 0;
#else
            return (*value ? setenv(name, value, 1) : unsetenv(name)) == 0;
#endif
        }
    }

    TEST(IcicleCookieBranch, ExactDestinyCheckerBranchesAgreeWithInterpreter)
    {
        // These are the unmodified bytes at destiny2.exe RVA 0x187c480. Mapping the original
        // guest VAs keeps the RIP-relative cookie load and final failure jump exact as well.
        constexpr uint64_t checker = 0x14187c480;
        constexpr uint64_t cookie_address = 0x1420a9a88;
        constexpr uint64_t failure = 0x14187d148;
        constexpr uint64_t success = checker + 0x100;
        constexpr uint64_t stack = 0x7000000000;
        constexpr std::array<uint8_t, 33> code{
            0x48, 0x3b, 0x0d, 0x01, 0xd6, 0x82, 0x00, // cmp rcx, [rip+0x82d601]
            0xf2, 0x75, 0x12,                         // bnd jne failure jump
            0x48, 0xc1, 0xc1, 0x10,                   // rol rcx, 16
            0x66, 0xf7, 0xc1, 0xff, 0xff,             // test cx, 0xffff
            0xf2, 0x75, 0x02,                         // bnd jne restore-and-fail
            0xf2, 0xc3,                               // bnd ret
            0x48, 0xc1, 0xc9, 0x10,                   // ror rcx, 16
            0xe9, 0xa7, 0x0c, 0x00, 0x00,             // jmp failure
        };
        constexpr std::array<uint8_t, 2> loop{0xeb, 0xfe};

        struct case_data
        {
            const char* name;
            uint64_t cookie;
            uint64_t supplied;
            uint64_t expected_pc;
            uint64_t expected_rcx;
            bool zero_flag;
            bool returned;
        };

        constexpr uint64_t valid_cookie = 0x0000123456789abc;
        constexpr uint64_t high_cookie = 0xbeef123456789abc;
        constexpr std::array cases{
            case_data{"matching cookie returns", valid_cookie, valid_cookie, success, std::rotl(valid_cookie, 16), true, true},
            case_data{"mismatch takes first BND JNE", valid_cookie, valid_cookie ^ 1, failure, valid_cookie ^ 1, false, false},
            case_data{"high bits take second BND JNE", high_cookie, high_cookie, failure, high_cookie, false, false},
        };

        const auto saved = [](const char* name) {
            const char* value = std::getenv(name);
            return value ? std::string(value) : std::string{};
        };
        const auto old_jit = saved("SOGEN_ICICLE_JIT");
        const auto old_hook = saved("SOGEN_ICICLE_INSTRUCTION_HOOK");
        const auto old_profile = saved("SOGEN_ICICLE_JIT_PROFILE");
        const auto restore = utils::finally([&] {
            set_environment("SOGEN_ICICLE_JIT", old_jit.c_str());
            set_environment("SOGEN_ICICLE_INSTRUCTION_HOOK", old_hook.c_str());
            set_environment("SOGEN_ICICLE_JIT_PROFILE", old_profile.c_str());
        });
        ASSERT_TRUE(set_environment("SOGEN_ICICLE_INSTRUCTION_HOOK", "0"));
        ASSERT_TRUE(set_environment("SOGEN_ICICLE_JIT_PROFILE", "1"));

        for (const bool jit : {false, true})
        {
            ASSERT_TRUE(set_environment("SOGEN_ICICLE_JIT", jit ? "1" : "0"));
            for (const auto& scenario : cases)
            {
                SCOPED_TRACE(testing::Message() << "jit=" << jit << " case=" << scenario.name);
                auto emu = create_x86_64_emulator(backend_type::icicle);
                memory_manager memory(*emu);
                ASSERT_TRUE(memory.allocate_memory(checker & ~0xfffULL, 0x2000, memory_permission::all));
                ASSERT_TRUE(memory.allocate_memory(cookie_address & ~0xfffULL, 0x1000, memory_permission::read_write));
                ASSERT_TRUE(memory.allocate_memory(stack, 0x1000, memory_permission::read_write));

                emu->write_memory(checker, code.data(), code.size());
                emu->write_memory(success, loop.data(), loop.size());
                emu->write_memory(failure, loop.data(), loop.size());
                emu->write_memory<uint64_t>(cookie_address, scenario.cookie);
                constexpr uint64_t initial_rsp = stack + 0x800;
                emu->write_memory<uint64_t>(initial_rsp, success);
                emu->reg(x86_register::rip, checker);
                emu->reg(x86_register::rcx, scenario.supplied);
                emu->reg(x86_register::rsp, initial_rsp);

                // Both outcomes land in a two-byte self-loop, so ample fuel permits JIT
                // compilation and execution without relying on a tiny step's fallback path.
                ASSERT_NO_THROW(emu->start(100));
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), scenario.expected_pc);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rcx), scenario.expected_rcx);
                EXPECT_EQ((emu->reg<uint32_t>(x86_register::eflags) & 0x40U) != 0, scenario.zero_flag);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rsp), initial_rsp + (scenario.returned ? 8 : 0));
                EXPECT_EQ(emu->read_memory<uint64_t>(cookie_address), scenario.cookie);

                emu->sync_worker_context(0); // publish the bridge counters after this single-vCPU quantum
                const auto profile = emu->jit_profile();
                ASSERT_EQ(profile.size(), 1U);
                if (jit)
                {
                    EXPECT_GT(profile.front().compile_calls, 0U);
                }
            }
        }
    }
}
