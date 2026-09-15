#include "emulation_test_utils.hpp"
#include <win_x86_64_gdb_stub_handler.hpp>

namespace sogen::test
{
    TEST(GdbOutputInterrupt, PendingOutputAndInterruptDoNotResumeGuest)
    {
        for (const bool single_step : {false, true})
        {
            SCOPED_TRACE(single_step);
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            auto win_emu = create_sample_emulator(std::move(settings));
            win_emu.setup_process_if_necessary();
            const auto address = win_emu.mod_manager.executable->entry_point;
            win_emu.emu().write_memory<uint8_t>(address, 0x90);
            win_emu.emu().reg(x86_register::rip, address);
            win_emu.callbacks.on_instruction = [&](uint64_t) { win_emu.stop(); };
            win_x86_64_gdb_stub_handler handler{win_emu};
            const auto before = win_emu.get_executed_instructions();
            const auto registers = win_emu.emu().save_registers();
            win_emu.callbacks.on_debug_string("guest output before interrupt");
            handler.on_interrupt();

            EXPECT_EQ(single_step ? handler.singlestep() : handler.run(), gdb_stub::action::output);
            EXPECT_EQ(win_emu.get_executed_instructions(), before);
            EXPECT_EQ(win_emu.emu().save_registers(), registers);
            EXPECT_EQ(handler.consume_debug_output(), "guest output before interrupt");
            EXPECT_EQ(single_step ? handler.singlestep() : handler.run(), gdb_stub::action::resume);
            EXPECT_EQ(win_emu.get_executed_instructions(), before);
            EXPECT_EQ(win_emu.emu().save_registers(), registers);

            EXPECT_EQ(handler.singlestep(), gdb_stub::action::resume);
            EXPECT_EQ(win_emu.get_executed_instructions(), before + 1);
        }
    }
}
