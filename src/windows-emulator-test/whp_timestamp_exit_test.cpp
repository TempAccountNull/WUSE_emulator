#include "emulation_test_utils.hpp"

#if defined(_WIN64) && !defined(__MINGW64__)
#include "../backends/whp-emulator/whp_instruction_exit.hpp"

#include <memory_manager.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sogen::test
{
    namespace
    {
        struct timestamp_encoding
        {
            const char* name;
            x86_hookable_instructions type;
            std::array<uint8_t, 4> bytes;
            size_t size;
        };

        constexpr std::array encodings{
            timestamp_encoding{"Rdtsc", x86_hookable_instructions::rdtsc, {0x0F, 0x31}, 2},
            timestamp_encoding{"Rdtscp", x86_hookable_instructions::rdtscp, {0x0F, 0x01, 0xF9}, 3},
            timestamp_encoding{"RexRdtsc", x86_hookable_instructions::rdtsc, {0x40, 0x0F, 0x31}, 3},
            timestamp_encoding{"RexRdtscp", x86_hookable_instructions::rdtscp, {0x40, 0x0F, 0x01, 0xF9}, 4},
            timestamp_encoding{"RexWRdtsc", x86_hookable_instructions::rdtsc, {0x48, 0x0F, 0x31}, 3},
            timestamp_encoding{"RexWRdtscp", x86_hookable_instructions::rdtscp, {0x48, 0x0F, 0x01, 0xF9}, 4},
        };

        class WhpTimestampExit : public testing::TestWithParam<timestamp_encoding>
        {
          protected:
            void SetUp() override
            {
                this->emu_ = create_x86_64_emulator(backend_type::whp);
                this->memory_ = std::make_unique<memory_manager>(*this->emu_);
                this->code_ = this->memory_->allocate_memory(0x1000, memory_permission::all);
                ASSERT_NE(this->code_, 0U);

                std::array<uint8_t, 0x100> guest{};
                guest.fill(0x90);
                std::copy_n(GetParam().bytes.begin(), GetParam().size, guest.begin());
                guest.back() = 0xF4;
                this->emu_->write_memory(this->code_, guest.data(), guest.size());
            }

            void TearDown() override
            {
                for (auto* hook : this->hooks_)
                {
                    this->emu_->delete_hook(hook);
                }
            }

            void reset(const uint64_t carry)
            {
                this->emu_->reg(x86_register::rip, this->code_);
                this->emu_->reg(x86_register::rflags, 0x202ULL | carry);
                this->emu_->reg(x86_register::rax, initial_rax);
                this->emu_->reg(x86_register::rdx, initial_rdx);
                this->emu_->reg(x86_register::rcx, initial_rcx);
                this->observed_ = false;
                this->observed_rip_ = 0;
                this->observed_carry_ = UINT64_MAX;
            }

            void observe(const uint64_t address)
            {
                auto* hook = this->emu_->hook_memory_execution_with_mode(
                    address, hook_interface::memory_execution_hook_mode::int3, [this](cpu_interface& cpu, uint64_t) {
                        this->observed_ = true;
                        this->observed_rip_ = this->emu_->reg<uint64_t>(x86_register::rip);
                        this->observed_carry_ = this->emu_->reg<uint64_t>(x86_register::rflags) & 1U;
                        cpu.stop();
                    });
                ASSERT_NE(hook, nullptr);
                this->hooks_.push_back(hook);
            }

            void hook_timestamp(instruction_hook_callback callback)
            {
                auto* hook = this->emu_->hook_instruction(GetParam().type, std::move(callback));
                ASSERT_NE(hook, nullptr);
                this->hooks_.push_back(hook);
            }

            bool is_rdtscp() const
            {
                return GetParam().type == x86_hookable_instructions::rdtscp;
            }

            void write_hook_outputs()
            {
                this->emu_->reg(x86_register::rax, hook_rax);
                this->emu_->reg(x86_register::rdx, hook_rdx);
                if (this->is_rdtscp())
                {
                    this->emu_->reg(x86_register::rcx, hook_rcx);
                }
            }

            void expect_hook_outputs() const
            {
                EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rax), hook_rax);
                EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rdx), hook_rdx);
                EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rcx), this->is_rdtscp() ? hook_rcx : initial_rcx);
            }

            void expect_default_outputs() const
            {
                EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rax) >> 32, 0U);
                EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rdx) >> 32, 0U);
                if (this->is_rdtscp())
                {
                    EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rcx) >> 32, 0U);
                }
                else
                {
                    EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rcx), initial_rcx);
                }
            }

            void expect_observation(const uint64_t address, const uint64_t carry) const
            {
                EXPECT_TRUE(this->observed_);
                EXPECT_EQ(this->observed_rip_, address);
                EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rip), address);
                EXPECT_EQ(this->observed_carry_, carry);
                EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rflags) & 1U, carry);
            }

            static constexpr uint64_t initial_rax = 0x1111111100000001ULL;
            static constexpr uint64_t initial_rdx = 0x2222222200000002ULL;
            static constexpr uint64_t initial_rcx = 0x3333333300000003ULL;
            static constexpr uint64_t hook_rax = 0x4444444400000004ULL;
            static constexpr uint64_t hook_rdx = 0x5555555500000005ULL;
            static constexpr uint64_t hook_rcx = 0x6666666600000006ULL;

            std::unique_ptr<x86_64_emulator> emu_{};
            std::unique_ptr<memory_manager> memory_{};
            std::vector<emulator_hook*> hooks_{};
            uint64_t code_{};
            bool observed_{};
            uint64_t observed_rip_{};
            uint64_t observed_carry_{};
        };
    }

    TEST_P(WhpTimestampExit, DefaultExecutionPreservesCarryAndReachesNextInstruction)
    {
        const auto next = this->code_ + GetParam().size;
        this->observe(next);
        for (const uint64_t carry : {0U, 1U})
        {
            SCOPED_TRACE(carry);
            this->reset(carry);
            ASSERT_NO_THROW(this->emu_->start(0));
            this->expect_observation(next, carry);
            this->expect_default_outputs();
        }
    }

    TEST_P(WhpTimestampExit, RunHookUsesDefaultOutputsAndDecodedInstructionLength)
    {
        size_t hits = 0;
        const auto next = this->code_ + GetParam().size;
        this->observe(next);
        this->hook_timestamp([this, &hits](cpu_interface& cpu, uint64_t) {
            EXPECT_EQ(cpu.index(), 0U);
            EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rip), this->code_);
            ++hits;
            this->write_hook_outputs();
            return instruction_hook_continuation::run_instruction;
        });
        for (const uint64_t carry : {0U, 1U})
        {
            SCOPED_TRACE(carry);
            this->reset(carry);
            ASSERT_NO_THROW(this->emu_->start(0));
            this->expect_observation(next, carry);
            this->expect_default_outputs();
        }
        EXPECT_EQ(hits, 2U);
    }

    TEST_P(WhpTimestampExit, SkipHookPreservesOutputsAndDecodedInstructionLength)
    {
        size_t hits = 0;
        const auto next = this->code_ + GetParam().size;
        this->observe(next);
        this->hook_timestamp([this, &hits](cpu_interface&, uint64_t) {
            ++hits;
            this->write_hook_outputs();
            return instruction_hook_continuation::skip_instruction;
        });
        for (const uint64_t carry : {0U, 1U})
        {
            SCOPED_TRACE(carry);
            this->reset(carry);
            ASSERT_NO_THROW(this->emu_->start(0));
            this->expect_observation(next, carry);
            this->expect_hook_outputs();
        }
        EXPECT_EQ(hits, 2U);
    }

    TEST_P(WhpTimestampExit, RunHookRedirectPreservesDefaultOutputsAndDestination)
    {
        size_t hits = 0;
        const auto destination = this->code_ + 0x80;
        this->observe(destination);
        this->hook_timestamp([this, destination, &hits](cpu_interface&, uint64_t) {
            ++hits;
            this->write_hook_outputs();
            this->emu_->reg(x86_register::rip, destination);
            return instruction_hook_continuation::run_instruction;
        });
        for (const uint64_t carry : {0U, 1U})
        {
            SCOPED_TRACE(carry);
            this->reset(carry);
            ASSERT_NO_THROW(this->emu_->start(0));
            this->expect_observation(destination, carry);
            this->expect_default_outputs();
        }
        EXPECT_EQ(hits, 2U);
    }

    TEST_P(WhpTimestampExit, SkipHookRedirectIsNotAdvanced)
    {
        size_t hits = 0;
        const auto destination = this->code_ + 0x80;
        this->observe(destination);
        this->hook_timestamp([this, destination, &hits](cpu_interface&, uint64_t) {
            ++hits;
            this->write_hook_outputs();
            this->emu_->reg(x86_register::rip, destination);
            return instruction_hook_continuation::skip_instruction;
        });
        for (const uint64_t carry : {0U, 1U})
        {
            SCOPED_TRACE(carry);
            this->reset(carry);
            ASSERT_NO_THROW(this->emu_->start(0));
            this->expect_observation(destination, carry);
            this->expect_hook_outputs();
        }
        EXPECT_EQ(hits, 2U);
    }

    TEST_P(WhpTimestampExit, FinalizedHookPreservesOutputsAndRedirect)
    {
        size_t hits = 0;
        const auto destination = this->code_ + 0x80;
        this->observe(destination);
        this->hook_timestamp([this, destination, &hits](cpu_interface&, uint64_t) {
            ++hits;
            this->write_hook_outputs();
            this->emu_->reg(x86_register::rip, destination);
            return instruction_hook_continuation::finalized_instruction_pointer;
        });
        for (const uint64_t carry : {0U, 1U})
        {
            SCOPED_TRACE(carry);
            this->reset(carry);
            ASSERT_NO_THROW(this->emu_->start(0));
            this->expect_observation(destination, carry);
            this->expect_hook_outputs();
        }
        EXPECT_EQ(hits, 2U);
    }

    TEST_P(WhpTimestampExit, StopInsideSkipHookCompletesOnlyTimestampInstruction)
    {
        size_t hits = 0;
        this->hook_timestamp([this, &hits](cpu_interface& cpu, uint64_t) {
            ++hits;
            this->write_hook_outputs();
            cpu.stop();
            return instruction_hook_continuation::skip_instruction;
        });
        for (const uint64_t carry : {0U, 1U})
        {
            SCOPED_TRACE(carry);
            this->reset(carry);
            ASSERT_NO_THROW(this->emu_->start(0));
            EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rip), this->code_ + GetParam().size);
            EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rflags) & 1U, carry);
            this->expect_hook_outputs();
        }
        EXPECT_EQ(hits, 2U);
    }

    TEST_P(WhpTimestampExit, FinalizedHookMayKeepOriginalInstructionPointer)
    {
        size_t hits = 0;
        this->hook_timestamp([this, &hits](cpu_interface& cpu, uint64_t) {
            ++hits;
            this->write_hook_outputs();
            cpu.stop();
            return instruction_hook_continuation::finalized_instruction_pointer;
        });
        for (const uint64_t carry : {0U, 1U})
        {
            SCOPED_TRACE(carry);
            this->reset(carry);
            ASSERT_NO_THROW(this->emu_->start(0));
            EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rip), this->code_);
            EXPECT_EQ(this->emu_->reg<uint64_t>(x86_register::rflags) & 1U, carry);
            this->expect_hook_outputs();
        }
        EXPECT_EQ(hits, 2U);
    }

    INSTANTIATE_TEST_SUITE_P(Encodings, WhpTimestampExit, testing::ValuesIn(encodings),
                             [](const testing::TestParamInfo<timestamp_encoding>& info) { return std::string{info.param.name}; });

    TEST(WhpTimestampExitLength, RejectsMissingAndTruncatedSdkExitLengths)
    {
        WHV_RUN_VP_EXIT_CONTEXT context{};
        context.ExitReason = WHvRunVpExitReasonX64Rdtsc;
        for (const uint8_t length : std::array<uint8_t, 2>{0, 1})
        {
            context.VpContext.InstructionLength = length;
            EXPECT_THROW((void)whp::detail::require_timestamp_instruction_length(context), std::runtime_error);
        }
        context.VpContext.InstructionLength = 2;
        EXPECT_EQ(whp::detail::require_timestamp_instruction_length(context), 2U);
        context.VpContext.InstructionLength = 3;
        EXPECT_EQ(whp::detail::require_timestamp_instruction_length(context), 3U);
        context.ReadTsc.RdtscInfo.IsRdtscp = 1;
        for (const uint8_t length : std::array<uint8_t, 3>{0, 1, 2})
        {
            context.VpContext.InstructionLength = length;
            EXPECT_THROW((void)whp::detail::require_timestamp_instruction_length(context), std::runtime_error);
        }
        context.VpContext.InstructionLength = 3;
        EXPECT_EQ(whp::detail::require_timestamp_instruction_length(context), 3U);
        context.VpContext.InstructionLength = 4;
        EXPECT_EQ(whp::detail::require_timestamp_instruction_length(context), 4U);
        context.VpContext.InstructionLength = 15;
        EXPECT_EQ(whp::detail::require_timestamp_instruction_length(context), 15U);
    }
}
#endif
