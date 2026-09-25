#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>

#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace sogen::test
{
    namespace
    {
        bool set_lean_test_env(const char* name, const char* value)
        {
#ifdef OS_WINDOWS
            return _putenv_s(name, value) == 0;
#else
            return (*value ? setenv(name, value, 1) : unsetenv(name)) == 0;
#endif
        }

        class lean_test_environment
        {
          public:
            lean_test_environment()
                : old_jit_(saved("SOGEN_ICICLE_JIT")),
                  old_hook_(saved("SOGEN_ICICLE_INSTRUCTION_HOOK")),
                  old_barrier_(saved("SOGEN_ICICLE_LEAN_BARRIER")),
                  old_profile_(saved("SOGEN_ICICLE_JIT_PROFILE"))
            {
                EXPECT_TRUE(set_lean_test_env("SOGEN_ICICLE_INSTRUCTION_HOOK", "0"));
                EXPECT_TRUE(set_lean_test_env("SOGEN_ICICLE_LEAN_BARRIER", "1"));
                EXPECT_TRUE(set_lean_test_env("SOGEN_ICICLE_JIT_PROFILE", "1"));
            }

            ~lean_test_environment()
            {
                set_lean_test_env("SOGEN_ICICLE_JIT", old_jit_.c_str());
                set_lean_test_env("SOGEN_ICICLE_INSTRUCTION_HOOK", old_hook_.c_str());
                set_lean_test_env("SOGEN_ICICLE_LEAN_BARRIER", old_barrier_.c_str());
                set_lean_test_env("SOGEN_ICICLE_JIT_PROFILE", old_profile_.c_str());
            }

            bool jit(const bool enabled) const
            {
                return set_lean_test_env("SOGEN_ICICLE_JIT", enabled ? "1" : "0");
            }

          private:
            static std::string saved(const char* name)
            {
                const char* value = std::getenv(name);
                return value ? value : "";
            }

            std::string old_jit_, old_hook_, old_barrier_, old_profile_;
        };

        void expect_compilation_if_jit(x86_64_emulator& emu, const bool jit)
        {
            emu.sync_worker_context(0);
            const auto profile = emu.jit_profile();
            ASSERT_EQ(profile.size(), 1U);
            if (jit)
            {
                EXPECT_GT(profile.front().compile_calls, 0U);
            }
        }
    }

    // The current lean path uses a no-op hook after each guest instruction as an opaque
    // JIT register-state barrier. Keep these cases reusable for an opt-in faster lowering.
    TEST(IcicleLeanBarrier, RegisterAndFlagsFlowAcrossInstructions)
    {
        lean_test_environment environment;
        constexpr uint64_t original = 0x1122334455667788;
        constexpr std::array<uint8_t, 47> code{
            0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // mov rax, original
            0x48, 0xbb, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // mov rbx, 0x0101...
            0x48, 0x01, 0xd8,                                           // add rax, rbx
            0x48, 0x29, 0xd8,                                           // sub rax, rbx
            0xb4, 0x77,                                                 // mov ah, 0x77
            0x48, 0x39, 0xd0,                                           // cmp rax, rdx
            0x75, 0x07,                                                 // jne failure
            0xb9, 0x42, 0x00, 0x00, 0x00,                               // mov ecx, 0x42
            0xeb, 0x05,                                                 // jmp done
            0xb9, 0xad, 0x0b, 0x00, 0x00,                               // failure: mov ecx, 0xbad
            0xeb, 0xfe,                                                 // done: jmp done
        };
        for (const bool jit : {false, true})
        {
            ASSERT_TRUE(environment.jit(jit));
            for (const bool equal : {false, true})
            {
                SCOPED_TRACE(testing::Message() << "jit=" << jit << " equal=" << equal);
                auto emu = create_x86_64_emulator(backend_type::icicle);
                memory_manager memory(*emu);
                const auto ip = memory.allocate_memory(0x1000, memory_permission::all);
                ASSERT_NE(ip, 0U);
                emu->write_memory(ip, code.data(), code.size());
                emu->reg(x86_register::rip, ip);
                emu->reg(x86_register::rdx, original ^ (equal ? 0 : 1));
                ASSERT_NO_THROW(emu->start(100));
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + 45);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), original);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rbx), 0x0101010101010101ULL);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rcx), equal ? 0x42U : 0xbadU);
                EXPECT_EQ((emu->reg<uint32_t>(x86_register::eflags) & 0x40U) != 0, equal);
                expect_compilation_if_jit(*emu, jit);
            }
        }
    }

    TEST(IcicleLeanBarrier, FaultSeesFlushedRegistersAndInstructionPc)
    {
        lean_test_environment environment;
        constexpr uint64_t value = 0x123456789abcdef0;
        constexpr uint64_t unmapped = 0x50000000;
        constexpr std::array<uint8_t, 25> code{
            0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, // mov rax, value
            0x48, 0xbb, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, // mov rbx, unmapped
            0x48, 0x8b, 0x03,                                           // mov rax, [rbx]
            0xeb, 0xfe,                                                 // jmp $
        };
        for (const bool jit : {false, true})
        {
            ASSERT_TRUE(environment.jit(jit));
            SCOPED_TRACE(testing::Message() << "jit=" << jit);
            auto emu = create_x86_64_emulator(backend_type::icicle);
            memory_manager memory(*emu);
            const auto ip = memory.allocate_memory(0x1000, memory_permission::all);
            ASSERT_NE(ip, 0U);
            emu->write_memory(ip, code.data(), code.size());
            emu->reg(x86_register::rip, ip);
            size_t faults{};
            emu->hook_memory_violation([&](cpu_interface&, uint64_t address, size_t, memory_operation operation, memory_violation_type) {
                ++faults;
                EXPECT_EQ(address, unmapped);
                EXPECT_EQ(operation, memory_operation::read);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), value);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rbx), unmapped);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + 20);
                return memory_violation_continuation::stop;
            });
            EXPECT_THROW(emu->start(100), std::runtime_error);
            EXPECT_EQ(faults, 1U);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), value);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), ip + 20);
            expect_compilation_if_jit(*emu, jit);
        }
    }

    TEST(IcicleLeanBarrier, GuestInterruptStopSkipsFollowingInstruction)
    {
        lean_test_environment environment;
        constexpr uint64_t value = 0x123456789abcdef0;
        constexpr std::array<uint8_t, 16> code{
            0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, // mov rax, value
            0xcc,                                                       // int3
            0x48, 0xff, 0xc0,                                           // inc rax (must not execute)
            0xeb, 0xfe,                                                 // jmp $
        };
        for (const bool jit : {false, true})
        {
            ASSERT_TRUE(environment.jit(jit));
            SCOPED_TRACE(testing::Message() << "jit=" << jit);
            auto emu = create_x86_64_emulator(backend_type::icicle);
            memory_manager memory(*emu);
            const auto ip = memory.allocate_memory(0x1000, memory_permission::all);
            ASSERT_NE(ip, 0U);
            emu->write_memory(ip, code.data(), code.size());
            emu->reg(x86_register::rip, ip);
            size_t interrupts{};
            emu->hook_interrupt([&](cpu_interface& cpu, const int number) {
                ++interrupts;
                EXPECT_EQ(number, 3);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), value);
                cpu.stop();
            });
            ASSERT_NO_THROW(emu->start(100));
            EXPECT_EQ(interrupts, 1U);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), value);
            expect_compilation_if_jit(*emu, jit);
        }
    }
}
