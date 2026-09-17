#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <array>
#include <vector>

namespace sogen::test
{
    // Host exceptions thrown inside hooks used to unwind through icicle's Rust frames and abort the
    // process (analyzer.exe.40620.dmp: "panic in a function that cannot unwind"). They must instead
    // stop the emulator and surface from start().
    class IcicleHookExceptions : public testing::Test
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x1000, memory_permission::all);
            ASSERT_NE(code, 0U);
        }

        void write_code(const std::vector<uint8_t>& bytes)
        {
            emu->write_memory(code, bytes.data(), bytes.size());
            emu->reg(x86_register::rip, code);
        }
    };

    TEST_F(IcicleHookExceptions, ExecutionHookExceptionStopsAndRethrowsFromStart)
    {
        write_code({0x90, 0x90, 0x90, 0x90, 0x90, 0x90});
        std::vector<uint64_t> seen;
        bool armed = true;
        emu->hook_memory_execution([&](cpu_interface&, const uint64_t address) {
            seen.push_back(address);
            if (armed && address == code + 2)
            {
                armed = false;
                throw std::runtime_error("host hook failure");
            }
        });

        try
        {
            emu->start(6);
            FAIL() << "the hook exception must surface from start()";
        }
        catch (const std::runtime_error& e)
        {
            EXPECT_STREQ(e.what(), "host hook failure");
        }
        EXPECT_EQ(seen, (std::vector<uint64_t>{code, code + 1, code + 2}));

        // The emulator remains usable and continues from where it stopped.
        const auto resumed_at = emu->read_instruction_pointer();
        EXPECT_TRUE(resumed_at == code + 2 || resumed_at == code + 3) << std::hex << resumed_at;
        emu->start(2);
        EXPECT_GE(seen.size(), 5U);
    }

    TEST_F(IcicleHookExceptions, InstructionHookExceptionIsDeferredAcrossTheBridge)
    {
        write_code({0x0f, 0x31, 0x90, 0x90}); // rdtsc; nop; nop
        size_t calls{};
        emu->hook_instruction(x86_hookable_instructions::rdtsc,
                              [&](cpu_interface&, uint64_t) -> instruction_hook_continuation {
                                  ++calls;
                                  throw std::runtime_error("rdtsc hook failure");
                              });
        EXPECT_THROW(emu->start(3), std::runtime_error);
        EXPECT_EQ(calls, 1U);
    }

    TEST_F(IcicleHookExceptions, MemoryHookExceptionIsDeferredAcrossTheBridge)
    {
        // mov eax, [rip+0x100]; nop  -> reads from a hooked page range inside the same allocation.
        const uint64_t data = code + 0x800;
        emu->write_memory<uint32_t>(data, 0x11223344);
        write_code({0x8b, 0x05, 0xfa, 0x07, 0x00, 0x00, 0x90}); // mov eax, [rip+0x7fa] (rip after = code+6 -> code+0x800)
        emu->hook_memory_read(data, 4, [&](cpu_interface&, uint64_t, const void*, size_t) {
            throw std::runtime_error("read hook failure"); //
        });
        EXPECT_THROW(emu->start(2), std::runtime_error);
    }
}
