#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <memory_manager.hpp>
#include <gtest/gtest.h>
#include <windows_emulator.hpp>
#include <map>
#include <unordered_set>
#include "../windows-analyzer/object_watching.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>

namespace sogen::test
{
    TEST(AnalyzerRawHookDispatch, ConcurrentObjectReadsUseActingVcpuAndSerializeCallbacks)
    {
        emulator_settings settings{.disable_logging = true, .use_instruction_precision = false};
        settings.load_registry = false;
        emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<null_ui_backend>();
        windows_emulator win{icicle::create_x86_64_emulator(2), settings, {}, std::move(interfaces)};

        const auto code = win.memory.allocate_memory(0x1000, memory_permission::all);
        const auto object = win.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(code, 0U);
        ASSERT_NE(object, 0U);

        std::array<uint8_t, 15> instructions{0x48, 0xB8};
        std::memcpy(instructions.data() + 2, &object, sizeof(object));
        instructions[10] = 0x48;
        instructions[11] = 0x8B;
        instructions[12] = 0x08;
        instructions[13] = 0xEB;
        instructions[14] = 0xFB;
        win.memory.write_memory(code, instructions.data(), instructions.size());

        std::array<std::atomic<uint64_t>, 2> hits{};
        std::atomic<unsigned> active_callbacks{};
        std::atomic<unsigned> overlaps{};
        std::atomic<unsigned> wrong_cpu{};
        const std::set<std::string, std::less<>> modules{};
        analysis_hook_profile profile{};
        profile.enabled = true;
        ASSERT_NE(watch_object<PEB64>(win, modules, object, true,
                                      [&](const object_access_info&) {
                                          const auto cpu_index = win.active_cpu().index();
                                          if (cpu_index >= hits.size())
                                          {
                                              ++wrong_cpu;
                                              return;
                                          }
                                          if (active_callbacks.fetch_add(1) != 0)
                                          {
                                              ++overlaps;
                                          }
                                          ++hits[cpu_index];
                                          std::this_thread::yield();
                                          --active_callbacks;
                                      }, &profile),
                  nullptr);

        std::atomic<unsigned> ready{};
        std::atomic<bool> go{};
        std::array<std::string, 2> errors{};
        const auto run = [&](const size_t index) {
            auto& cpu = win.vcpu(index).cpu;
            cpu.reg(x86_register::rip, code);
            ++ready;
            while (!go.load())
            {
                std::this_thread::yield();
            }
            try
            {
                cpu.start(4096);
            }
            catch (const std::exception& error)
            {
                errors[index] = error.what();
            }
        };
        std::thread first(run, 0);
        std::thread second(run, 1);
        while (ready.load() != 2)
        {
            std::this_thread::yield();
        }
        go = true;
        first.join();
        second.join();

        EXPECT_TRUE(errors[0].empty()) << errors[0];
        EXPECT_TRUE(errors[1].empty()) << errors[1];
        EXPECT_GT(hits[0].load(), 0U);
        EXPECT_GT(hits[1].load(), 0U);
        EXPECT_EQ(wrong_cpu.load(), 0U);
        EXPECT_EQ(overlaps.load(), 0U);
        EXPECT_GT(profile.object_callback.samples.load(), 0U);
        EXPECT_EQ(profile.object_lock_wait.samples.load(), profile.object_callback.samples.load());
        EXPECT_LE(profile.object_lock_wait.max_nanos.load(), profile.object_callback.max_nanos.load());
    }
}
