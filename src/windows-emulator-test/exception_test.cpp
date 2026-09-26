#include "emulation_test_utils.hpp"

namespace sogen::test
{
    TEST(ExceptionTest, SecondChanceExceptionCodeBecomesExitStatus)
    {
        constexpr NTSTATUS sample_fail_fast_code = 0xE0001234;

        auto emu = create_sample_emulator({.fail_fast = true});
        emu.start();

        ASSERT_TERMINATED_WITH_STATUS(emu, sample_fail_fast_code);
    }

    TEST(ExceptionTest, FirstChanceTraceRetainsEarliestAndLatestWithoutFlooding)
    {
        auto emu = create_sample_emulator(emulator_settings{});
        emu.setup_process_if_necessary();
        auto& cpu = emu.vcpu(0).cpu;
        const auto rip = emu.mod_manager.executable->entry_point;
        cpu.reg(x86_register::rax, uint64_t{0x12345678});

        for (uint32_t i = 0; i < 48; ++i)
        {
            emu.record_exception_trace({.status = STATUS_ACCESS_VIOLATION, .tid = 8, .vcpu = 0, .rip = rip, .info = i}, cpu);
        }
        for (uint32_t i = 0; i < 1030; ++i)
        {
            emu.record_exception_trace({.status = STATUS_BREAKPOINT, .tid = 8, .vcpu = 0, .rip = rip}, cpu);
        }

        const auto trace = emu.exception_trace_snapshot();
        ASSERT_EQ(emu.exception_trace_non_debug_count(), 48U);
        ASSERT_EQ(emu.exception_trace_debug_count(), 1030U);
        ASSERT_EQ(trace.size(), 45U); // first 8, last 32, first 4 debug, one sampled debug
        EXPECT_EQ(trace[0].ordinal, 1U);
        EXPECT_EQ(trace[7].ordinal, 8U);
        EXPECT_EQ(trace[8].ordinal, 17U);
        EXPECT_EQ(trace[39].ordinal, 48U);
        EXPECT_EQ(trace[40].ordinal, 49U);
        EXPECT_EQ(trace.back().ordinal, 1073U);
        EXPECT_EQ(trace[0].gprs[0], 0x12345678U);
        EXPECT_EQ(trace[0].module_name.data(), std::string("test-sample.exe"));
        EXPECT_EQ(trace[0].module_rva, rip - trace[0].module_base);
        EXPECT_GT(trace[0].readable_code_bytes, 0U);
        EXPECT_LE(trace[0].readable_stack_words, 16U);
    }
} // namespace sogen::test
