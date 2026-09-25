#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <memory_manager.hpp>
#include <gtest/gtest.h>
#include <utils/finally.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
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

        std::string environment_value(const char* name)
        {
            const char* value = std::getenv(name);
            return value ? value : "";
        }
    } // namespace

    TEST(IcicleSmpDataCoherence, ReinitializedNonExecutableCookieReachesWarmPeerJit)
    {
        const auto old_jit = environment_value("SOGEN_ICICLE_JIT");
        const auto old_hook = environment_value("SOGEN_ICICLE_INSTRUCTION_HOOK");
        const auto old_profile = environment_value("SOGEN_ICICLE_JIT_PROFILE");
        const auto restore = utils::finally([&] {
            set_environment("SOGEN_ICICLE_JIT", old_jit.c_str());
            set_environment("SOGEN_ICICLE_INSTRUCTION_HOOK", old_hook.c_str());
            set_environment("SOGEN_ICICLE_JIT_PROFILE", old_profile.c_str());
        });
        ASSERT_TRUE(set_environment("SOGEN_ICICLE_JIT", "1"));
        ASSERT_TRUE(set_environment("SOGEN_ICICLE_INSTRUCTION_HOOK", "0"));
        ASSERT_TRUE(set_environment("SOGEN_ICICLE_JIT_PROFILE", "1"));

        auto emu = icicle::create_x86_64_emulator(2);
        ASSERT_EQ(emu->vcpu_count(), 2U);
        memory_manager memory(*emu);
        const auto reader_page = memory.allocate_memory(0x1000, memory_permission::all);
        const auto writer_page = memory.allocate_memory(0x1000, memory_permission::all);
        const auto cookie_page = memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(reader_page, 0U);
        ASSERT_NE(writer_page, 0U);
        ASSERT_NE(cookie_page, 0U);
        const auto cookie_address = cookie_page + 0x88;

        // mov rax, [moffs64]; jmp to the load. The data page remains non-executable.
        std::array<uint8_t, 12> reader{0x48, 0xA1};
        std::memcpy(reader.data() + 2, &cookie_address, sizeof(cookie_address));
        reader[10] = 0xEB;
        reader[11] = 0xF4;
        emu->write_memory(reader_page, reader.data(), reader.size());

        // mov [moffs64], rax; jmp $: model the guest's later cookie initialization.
        std::array<uint8_t, 12> writer{0x48, 0xA3};
        std::memcpy(writer.data() + 2, &cookie_address, sizeof(cookie_address));
        writer[10] = 0xEB;
        writer[11] = 0xFE;
        emu->write_memory(writer_page, writer.data(), writer.size());

        constexpr uint64_t initial_cookie = 0x00002b992ddfa232;
        constexpr uint64_t initialized_cookie = 0x0000ea20ed3e4a5b;
        emu->write_memory(cookie_address, &initial_cookie, sizeof(initial_cookie));

        for (size_t i = 0; i < 2; ++i)
        {
            auto& cpu = emu->get_cpu(i);
            cpu.reg(x86_register::rip, reader_page);
            cpu.start(2);
            ASSERT_EQ(cpu.reg(x86_register::rax), initial_cookie) << "vCPU " << i << " missed the initial load";
        }
        for (size_t i = 0; i < 2; ++i)
        {
            emu->sync_worker_context(i);
        }
        const auto profile = emu->jit_profile();
        ASSERT_EQ(profile.size(), 2U);
        EXPECT_GT(profile[0].compile_calls, 0U);
        EXPECT_GT(profile[1].compile_calls, 0U);

        auto& writer_cpu = emu->get_cpu(0);
        writer_cpu.reg(x86_register::rax, initialized_cookie);
        writer_cpu.reg(x86_register::rip, writer_page);
        writer_cpu.start(2);

        uint64_t host_cookie{};
        emu->read_memory(cookie_address, &host_cookie, sizeof(host_cookie));
        ASSERT_EQ(host_cookie, initialized_cookie) << "guest store did not update the host API view";

        auto& peer = emu->get_cpu(1);
        peer.reg(x86_register::rax, 0);
        peer.reg(x86_register::rip, reader_page);
        peer.start(2);
        EXPECT_EQ(peer.reg(x86_register::rax), initialized_cookie)
            << "peer JIT load read the pre-initialization value from shared non-executable data";

        writer_cpu.reg(x86_register::rax, 0);
        writer_cpu.reg(x86_register::rip, reader_page);
        writer_cpu.start(2);
        EXPECT_EQ(writer_cpu.reg(x86_register::rax), initialized_cookie);
    }
} // namespace sogen::test
