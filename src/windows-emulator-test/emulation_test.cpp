#include "emulation_test_utils.hpp"

namespace sogen::test
{
    TEST(EmulationTest, BasicEmulationWorks)
    {
        auto emu = create_sample_emulator();
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);
    }

    TEST(EmulationTest, CountedEmulationWorks)
    {
        constexpr auto count = 200000;

        auto emu = create_sample_emulator();
        emu.start(count);

        ASSERT_EQ(emu.get_executed_instructions(), count);
    }

    TEST(EmulationTest, CountedEmulationIsAccurate)
    {
        auto emu = create_sample_emulator();
        emu.start();

        ASSERT_TERMINATED_SUCCESSFULLY(emu);

        const auto executedInstructions = emu.get_executed_instructions();

        auto new_emu = create_sample_emulator();

        constexpr auto offset = 1;
        const auto instructionsToExecute = executedInstructions - offset;

        new_emu.start(static_cast<size_t>(instructionsToExecute));

        ASSERT_EQ(new_emu.get_executed_instructions(), instructionsToExecute);
        ASSERT_NOT_TERMINATED(new_emu);

        new_emu.start(offset);

        ASSERT_TERMINATED_SUCCESSFULLY(new_emu);
        ASSERT_EQ(new_emu.get_executed_instructions(), executedInstructions);
    }

    TEST(EmulationTest, LeanBackendCountSurvivesSnapshotRestore)
    {
        emulator_settings settings{.disable_logging = true, .use_relative_time = false,
                                   .use_instruction_precision = false};
        auto emu = create_sample_emulator(settings);
        if (!emu.emu().is_stop_thread_safe() || !emu.emu().has_deterministic_instruction_count())
        {
            GTEST_SKIP() << "Requires a stop-safe backend with its own retired-instruction count";
        }

        emu.start(100);
        ASSERT_EQ(emu.get_executed_instructions(), 100U);
        emu.save_snapshot();

        emu.start(50);
        ASSERT_EQ(emu.get_executed_instructions(), 150U);

        // The regular restore resets Icicle's volatile icount. The Windows counter slot
        // now stores the cumulative total and becomes the base for post-restore work.
        emu.restore_snapshot();
        ASSERT_EQ(emu.get_executed_instructions(), 100U);
        emu.start(50);
        ASSERT_EQ(emu.get_executed_instructions(), 150U);
    }
} // namespace sogen::test
