#include "emulation_test_utils.hpp"
#include "../windows-analyzer/analysis.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"
#include "../windows-analyzer/jsonl_reporter.hpp"
#include <emulator_utils.hpp>
#include <syscall_utils.hpp>
#include <utils/io.hpp>
#include <utils/finally.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtTerminateThread(const syscall_context&, handle, NTSTATUS);
}

namespace sogen::test
{
    class AnalysisObservation : public testing::Test, public analysis_reporter
    {
      protected:
        windows_emulator win_emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        analysis_settings logging_settings{.verbose_logging = true};
        analysis_context analysis{.settings = &logging_settings, .win_emu = &win_emu, .reporters = {this}};
        std::vector<function_execution_event> calls{};
        std::vector<execution_progress_event> progress{};
        std::vector<import_read_event> imports{};
        std::vector<entry_point_execution_event> entries{};
        std::vector<foreign_code_transition_event> transitions{};
        std::vector<thread_terminated_event> terminated{};
        std::vector<memory_violation_event> violations{};
        std::vector<fast_fail_event> fast_fails{};
        uint64_t caller{};
        uint64_t callee{};
        uint64_t stack{};

        void report(const analysis_event& event) override
        {
            if (const auto* violation = std::get_if<memory_violation_event>(&event))
            {
                violations.push_back(*violation);
            }
            if (const auto* fast_fail = std::get_if<fast_fail_event>(&event))
            {
                fast_fails.push_back(*fast_fail);
            }
            if (const auto* call = std::get_if<function_execution_event>(&event))
            {
                calls.push_back(*call);
            }
            if (const auto* entry = std::get_if<entry_point_execution_event>(&event))
            {
                entries.push_back(*entry);
            }
            if (const auto* transition = std::get_if<foreign_code_transition_event>(&event))
            {
                transitions.push_back(*transition);
            }
            if (const auto* exit = std::get_if<thread_terminated_event>(&event))
            {
                terminated.push_back(*exit);
            }
            if (const auto* access = std::get_if<import_read_event>(&event))
            {
                imports.push_back(*access);
            }
            if (const auto* update = std::get_if<execution_progress_event>(&event))
            {
                progress.push_back(*update);
            }
        }

        void set_fault_stack_descriptor(const uint32_t base, const uint32_t limit, const uint8_t flags, const uint8_t access = 0xF3)
        {
            const auto table = segment_utils::read_descriptor_table(win_emu.emu(), x86_register::gdtr);
            ASSERT_TRUE(table);
            const segment_utils::raw_segment_descriptor descriptor{.limit_low = static_cast<uint16_t>(limit),
                                                                   .base_low = static_cast<uint16_t>(base),
                                                                   .base_mid = static_cast<uint8_t>(base >> 16),
                                                                   .access = access,
                                                                   .limit_high_flags = static_cast<uint8_t>(flags | ((limit >> 16) & 15)),
                                                                   .base_high = static_cast<uint8_t>(base >> 24)};
            win_emu.emu().write_memory(table->base + 0x58, &descriptor, sizeof(descriptor));
            win_emu.emu().reg(x86_register::cs, 0x23);
            win_emu.emu().reg(x86_register::ss, 0x5B);
        }

        void observe(const uint64_t previous, const uint64_t current)
        {
            auto& thread = win_emu.current_thread();
            thread.previous_ip = previous;
            thread.current_ip = current;
            thread.executed_instructions = 2;
            win_emu.emu().reg(x86_register::rip, current);
            win_emu.callbacks.on_instruction(current);
        }

        void SetUp() override
        {
            win_emu.setup_process_if_necessary();
            caller = win_emu.mod_manager.executable->entry_point + 0x100;
            callee = caller + 0x40;
            stack = win_emu.memory.allocate_memory(0x4000, memory_permission::read_write) + 0x2000;
            win_emu.mod_manager.executable->address_names[callee] = "WinVerifyTrust";
            win_emu.emu().reg(x86_register::rip, callee);
            win_emu.emu().reg(x86_register::rsp, stack);
            win_emu.emu().reg(x86_register::rax, 0x123456789ABCDEF0ULL);
            win_emu.emu().write_memory<uint64_t>(stack, caller + 2);
            register_analysis_callbacks(analysis);
        }
    };

    TEST_F(AnalysisObservation, ObjectAccessContextReusesResolvedModule)
    {
        win_emu.current_thread().previous_ip = caller;
        const auto expected = analysis.make_execution_context();
        const auto* module = win_emu.mod_manager.find_by_address(callee);
        ASSERT_NE(module, nullptr);

        const auto actual = analysis.make_execution_context(callee, module->name);
        EXPECT_EQ(actual.thread_id, expected.thread_id);
        EXPECT_EQ(actual.rip, expected.rip);
        EXPECT_EQ(actual.rip_module, expected.rip_module);
        EXPECT_EQ(actual.previous_ip, expected.previous_ip);
        EXPECT_EQ(actual.previous_ip_module, expected.previous_ip_module);
    }

    TEST_F(AnalysisObservation, FastFailCapturesBoundedGuestCodeAndRegisters)
    {
        const auto page = win_emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(page, 0U);
        const auto rip = page + 0x100;
        std::array<uint8_t, 40> bytes{};
        for (size_t i = 0; i < bytes.size(); ++i)
        {
            bytes[i] = static_cast<uint8_t>(i);
        }
        win_emu.emu().write_memory(rip - 24, bytes.data(), bytes.size());
        win_emu.emu().reg(x86_register::rip, rip);
        win_emu.emu().reg(x86_register::rcx, 2);
        win_emu.emu().reg(x86_register::r15, 0x12345678);
        win_emu.emu().write_memory<uint64_t>(stack, caller);
        win_emu.emu().write_memory<uint64_t>(stack + 8, caller); // Duplicate return address.
        win_emu.emu().write_memory<uint64_t>(stack + 16, 0xDEADBEEF); // Outside the main image.
        win_emu.emu().write_memory<uint64_t>(stack + 64, 0x123456789ULL); // Saved incoming RCX.
        const std::array<uint8_t, 32> caller_bytes{0x48, 0x8B, 0xC1, 0xE8};
        win_emu.emu().write_memory(caller - 16, caller_bytes.data(), caller_bytes.size());

        win_emu.callbacks.on_fast_fail(2);
        ASSERT_EQ(fast_fails.size(), 1U);
        const auto& event = fast_fails.front();
        EXPECT_EQ(event.fail_code, 2U);
        EXPECT_EQ(event.execution.rip, rip);
        EXPECT_EQ(event.code_base, rip - 24);
        EXPECT_EQ(event.readable_code_bytes, 40U);
        EXPECT_EQ(event.code_bytes.size(), 80U);
        EXPECT_EQ(event.code_bytes.substr(0, 8), "00010203");
        EXPECT_EQ(event.code_bytes.substr(48, 4), "1819");
        EXPECT_EQ(event.gprs[2], 2U);
        EXPECT_EQ(event.gprs[15], 0x12345678U);
        EXPECT_EQ(event.stack_words.front(), caller);
        ASSERT_EQ(event.caller_code.size(), 1U);
        EXPECT_EQ(event.caller_code.front().stack_word_index, 0U);
        EXPECT_EQ(event.caller_code.front().return_address, caller);
        EXPECT_EQ(event.caller_code.front().module_name, win_emu.mod_manager.executable->name);
        EXPECT_EQ(event.caller_code.front().module_base, win_emu.mod_manager.executable->image_base);
        EXPECT_EQ(event.caller_code.front().module_rva, caller - win_emu.mod_manager.executable->image_base);
        EXPECT_EQ(event.caller_code.front().code_base, caller - 16);
        EXPECT_EQ(event.caller_code.front().code_bytes.size(), 32U);
        EXPECT_EQ(event.caller_code.front().code_bytes.substr(0, 8), "488bc1e8");
        EXPECT_FALSE(event.expected_security_cookie_read);
        EXPECT_FALSE(event.supplied_security_cookie_read);
        EXPECT_FALSE(event.security_cookie_mismatch);
        EXPECT_EQ(event.gs_base, win_emu.emu().get_segment_base(x86_register::gs));
    }

    TEST_F(AnalysisObservation, FastFailResolvesExactDllRangeAndRawStackCandidate)
    {
        const auto* dll = win_emu.mod_manager.ntdll;
        ASSERT_NE(dll, nullptr);
        const auto rip = dll->entry_point;
        const auto candidate = rip + 0x20;
        ASSERT_TRUE(dll->contains(rip));
        ASSERT_TRUE(dll->contains(candidate));
        win_emu.emu().reg(x86_register::rip, rip);
        win_emu.emu().write_memory<uint64_t>(stack, candidate);
        win_emu.emu().write_memory<uint64_t>(stack + 8, candidate); // Duplicate is not another frame.
        win_emu.emu().write_memory<uint64_t>(stack + 16, UINT64_MAX - 0x1000); // No loaded-module match.

        win_emu.callbacks.on_fast_fail(7);
        ASSERT_EQ(fast_fails.size(), 1U);
        const auto& event = fast_fails.front();
        EXPECT_EQ(event.fail_code, 7U);
        EXPECT_EQ(event.rip_module_name, dll->name);
        EXPECT_EQ(event.rip_module_base, dll->image_base);
        EXPECT_EQ(event.rip_module_rva, rip - dll->image_base);
        ASSERT_EQ(event.caller_code.size(), 1U);
        const auto& raw = event.caller_code.front();
        EXPECT_EQ(raw.stack_word_index, 0U);
        EXPECT_EQ(raw.return_address, candidate);
        EXPECT_EQ(raw.module_name, dll->name);
        EXPECT_EQ(raw.module_base, dll->image_base);
        EXPECT_EQ(raw.module_rva, candidate - dll->image_base);
        EXPECT_EQ(raw.code_base, candidate - 16);
        EXPECT_EQ(raw.code_bytes.size(), 32U);
        EXPECT_LE(raw.readable_code_bytes, 16U);
    }

    TEST_F(AnalysisObservation, FastFailMarksUnreadableCodeBytesAcrossPageBoundary)
    {
        const auto page = win_emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        ASSERT_NE(page, 0U);
        ASSERT_TRUE(win_emu.memory.decommit_memory(page + 0x1000, 0x1000));
        const auto rip = page + 0xff8;
        std::array<uint8_t, 32> bytes{};
        bytes.fill(0xcc);
        win_emu.emu().write_memory(rip - 24, bytes.data(), bytes.size());
        win_emu.emu().reg(x86_register::rip, rip);

        win_emu.callbacks.on_fast_fail(2);
        ASSERT_EQ(fast_fails.size(), 1U);
        const auto& event = fast_fails.front();
        EXPECT_EQ(event.code_base, rip - 24);
        EXPECT_EQ(event.readable_code_bytes, 32U);
        EXPECT_EQ(event.code_bytes.size(), 80U);
        EXPECT_EQ(event.code_bytes.substr(0, 4), "cccc");
        EXPECT_EQ(event.code_bytes.substr(64), "????????????????");
    }

    TEST_F(AnalysisObservation, ExecuteFaultCapturesActualAndLastTrackedIpWithHighStackWithoutMutation)
    {
        const auto allocation = win_emu.memory.allocate_memory(0x1000, memory_permission::read_write, false, 0x100000000ULL);
        ASSERT_NE(allocation, 0U);
        const auto high_stack = allocation + 0x200;
        ASSERT_GT(high_stack, 0xFFFFFFFFULL);
        const std::array<uint8_t, 2> call{0xFF, 0xD0};
        win_emu.emu().write_memory(caller, call.data(), call.size());
        win_emu.emu().write_memory<uint64_t>(high_stack, caller + 2);
        auto& thread = win_emu.current_thread();
        thread.previous_ip = callee;
        thread.current_ip = caller;
        thread.executed_instructions = 3;
        win_emu.emu().reg(x86_register::rsp, high_stack);
        win_emu.emu().reg(x86_register::rip, 0ULL);
        win_emu.emu().reg(x86_register::rax, 0ULL);
        win_emu.emu().reg(x86_register::r15, 0x1234567887654321ULL);
        win_emu.emu().reg(x86_register::eflags, 0x246U);
        const auto before = win_emu.emu().save_registers();
        const auto layout = win_emu.memory.get_layout_version();
        win_emu.callbacks.on_memory_violate(0, 1, memory_operation::exec, memory_violation_type::unmapped);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(win_emu.memory.get_layout_version(), layout);
        EXPECT_EQ(win_emu.emu().read_memory<uint64_t>(high_stack), caller + 2);
        ASSERT_EQ(violations.size(), 1U);
        const auto& event = violations.front();
        EXPECT_EQ(event.execution.rip, 0U);
        EXPECT_EQ(event.execution.previous_ip, callee);
        EXPECT_EQ(event.actual_instruction.location.address, 0U);
        EXPECT_TRUE(event.actual_instruction.bytes_hex.empty());
        EXPECT_FALSE(event.actual_instruction.error.empty());
        EXPECT_TRUE(event.near_null_execute);
        ASSERT_TRUE(event.last_tracked_instruction);
        EXPECT_EQ(event.last_tracked_instruction->location.address, caller);
        EXPECT_EQ(event.last_tracked_instruction->bytes_hex, "ffd0");
        EXPECT_EQ(event.last_tracked_instruction->assembly, "call rax");
        EXPECT_EQ(event.last_tracked_instruction->location.module_base, win_emu.mod_manager.executable->image_base);
        EXPECT_EQ(event.last_tracked_instruction->location.module_rva, caller - win_emu.mod_manager.executable->image_base);
        EXPECT_EQ(event.code_bits, 64U);
        EXPECT_EQ(event.stack_slot.pointer_bits, 64U);
        EXPECT_EQ(event.stack_slot.address, high_stack);
        EXPECT_EQ(event.stack_slot.value, caller + 2);
        ASSERT_TRUE(event.stack_slot.value_location);
        ASSERT_TRUE(event.stack_slot.value_location->region);
        EXPECT_TRUE(event.stack_slot.value_location->region->committed);
        ASSERT_EQ(event.registers.size(), 20U);
        const auto value = [&](const std::string_view name) {
            for (const auto& reg : event.registers)
            {
                if (reg.name == name)
                {
                    return reg.value;
                }
            }
            return std::optional<uint64_t>{};
        };
        EXPECT_EQ(value("rsp"), high_stack);
        EXPECT_EQ(value("r15"), 0x1234567887654321ULL);
        EXPECT_EQ(value("eflags"), 0x246U);
        EXPECT_EQ(value("cs"), 0x33U);
        win_emu.emu().reg(x86_register::rip, callee);
        win_emu.emu().write_memory<uint64_t>(high_stack, 0);
        win_emu.emu().write_memory<uint8_t>(caller, 0x90);
        EXPECT_EQ(event.execution.rip, 0U);
        EXPECT_EQ(event.stack_slot.value, caller + 2);
        EXPECT_EQ(event.last_tracked_instruction->bytes_hex, "ffd0");
    }

    TEST_F(AnalysisObservation, PrivateExecuteFaultCapturesOneImmutableBoundedMemoryWindow)
    {
        const auto allocation = win_emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        ASSERT_NE(allocation, 0U);
        ASSERT_TRUE(win_emu.memory.decommit_memory(allocation + 0x1000, 0x1000));
        const auto rip = allocation + 0xFF8;
        const std::array<uint8_t, 8> bytes{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};
        win_emu.emu().write_memory(rip, bytes.data(), bytes.size());
        win_emu.emu().reg(x86_register::rip, rip);
        const auto before = win_emu.emu().save_registers();
        const auto layout = win_emu.memory.get_layout_version();
        win_emu.callbacks.on_memory_violate(rip, 1, memory_operation::exec, memory_violation_type::protection);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(win_emu.memory.get_layout_version(), layout);
        ASSERT_EQ(violations.size(), 1U);
        const auto first = violations.front();
        ASSERT_TRUE(first.private_execute_vcpu);
        EXPECT_EQ(*first.private_execute_vcpu, 0U);
        ASSERT_TRUE(first.actual_instruction.location.region);
        EXPECT_EQ(first.actual_instruction.location.region->allocation_base, allocation);
        ASSERT_EQ(first.private_execute_memory.size(), 8U);
        EXPECT_EQ(first.private_execute_memory.front().address, allocation + 0xFB0);
        EXPECT_EQ(first.private_execute_memory[4].address, allocation + 0xFF0);
        EXPECT_NE(first.private_execute_memory[4].bytes_hex.find("a0 a1 a2 a3 a4 a5 a6 a7"), std::string::npos);
        EXPECT_EQ(first.private_execute_memory[5].address, allocation + 0x1000);
        EXPECT_EQ(first.private_execute_memory[5].readable_bytes, 0U);
        EXPECT_NE(first.private_execute_memory[5].bytes_hex.find("?? ?? ??"), std::string::npos);

        win_emu.emu().write_memory<uint8_t>(rip, 0xCC);
        win_emu.emu().reg(x86_register::rip, allocation + 0x200);
        win_emu.callbacks.on_memory_violate(allocation + 0x200, 1, memory_operation::exec, memory_violation_type::protection);
        ASSERT_EQ(violations.size(), 2U);
        EXPECT_FALSE(violations.back().private_execute_vcpu);
        EXPECT_TRUE(violations.back().private_execute_memory.empty());
        EXPECT_NE(first.private_execute_memory[4].bytes_hex.find("a0 a1 a2 a3 a4 a5 a6 a7"), std::string::npos);

        std::string text;
        logger log;
        log.set_silent(true);
        log.set_sink([&](const color, const std::string_view line) { text += line; });
        auto console = create_console_reporter(log, {});
        console->report(first);
        EXPECT_NE(text.find("Private execute memory"), std::string::npos);
        EXPECT_NE(text.find("a0 a1 a2 a3 a4 a5 a6 a7"), std::string::npos);
        EXPECT_NE(text.find("?? ?? ??"), std::string::npos);

        const auto file = std::filesystem::temp_directory_path() / ("sogen-private-execute-" + std::to_string(getpid()) + ".jsonl");
        const auto cleanup = utils::finally([&] {
            std::error_code error;
            std::filesystem::remove(file, error);
        });
        auto jsonl = create_jsonl_reporter(file);
        jsonl->report(first);
        jsonl->flush();
        const auto saved = utils::io::read_file(file);
        const std::string json(reinterpret_cast<const char*>(saved.data()), saved.size());
        EXPECT_NE(json.find("\"privateExecuteVcpu\":0"), std::string::npos);
        EXPECT_NE(json.find("\"privateExecuteMemory\":"), std::string::npos);
        EXPECT_NE(json.find("a0 a1 a2 a3 a4 a5 a6 a7"), std::string::npos);
        EXPECT_NE(json.find("?? ?? ??"), std::string::npos);
    }

    TEST_F(AnalysisObservation, NullReadAndWriteFaultsDoNotInferACallOrReturnAddress)
    {
        const std::array<uint8_t, 3> load{0x48, 0x8B, 0x00};
        win_emu.emu().write_memory(caller, load.data(), load.size());
        win_emu.emu().reg(x86_register::rip, caller);
        win_emu.emu().write_memory<uint64_t>(stack, 0x1122334455667788ULL);
        const auto before = win_emu.emu().save_registers();
        for (const auto operation : {memory_operation::read, memory_operation::write})
        {
            win_emu.callbacks.on_memory_violate(0, 8, operation, memory_violation_type::unmapped);
            const auto& event = violations.back();
            EXPECT_FALSE(event.near_null_execute);
            EXPECT_EQ(event.actual_instruction.location.address, caller);
            EXPECT_EQ(event.actual_instruction.bytes_hex, "488b00");
            EXPECT_FALSE(event.actual_instruction.assembly.empty());
            EXPECT_EQ(event.stack_slot.value, 0x1122334455667788ULL);
        }
        EXPECT_EQ(win_emu.emu().save_registers(), before);
    }

    TEST_F(AnalysisObservation, CompatibilityFaultReadsOnlyTheLowEspFourByteSlot)
    {
        constexpr uint64_t low_stack = 0x40000000;
        ASSERT_TRUE(win_emu.memory.allocate_memory(low_stack, 0x1000, memory_permission::read_write));
        win_emu.emu().write_memory<uint64_t>(low_stack, 0xDEADBEEF12345678ULL);
        win_emu.emu().write_memory<uint16_t>(caller, 0x9040);
        win_emu.emu().reg(x86_register::cs, 0x23);
        ASSERT_TRUE(is_32bit_code_segment(win_emu.emu()));
        win_emu.emu().reg(x86_register::rsp, 0x9999999940000000ULL);
        win_emu.emu().reg(x86_register::rip, caller);
        const auto before = win_emu.emu().save_registers();
        win_emu.callbacks.on_memory_violate(0, 4, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        ASSERT_EQ(violations.size(), 1U);
        EXPECT_EQ(violations[0].code_bits, 32U);
        EXPECT_EQ(violations[0].actual_instruction.bytes_hex, "40");
        EXPECT_EQ(violations[0].actual_instruction.assembly, "inc eax");
        EXPECT_EQ(violations[0].stack_slot.pointer_bits, 32U);
        EXPECT_EQ(violations[0].stack_slot.address, low_stack);
        EXPECT_EQ(violations[0].stack_slot.value, 0x12345678U);
    }

    TEST_F(AnalysisObservation, CompatibilityStackAddsCurrentSsBaseWithoutChangingCpu)
    {
        constexpr uint32_t base = 0x46000000;
        ASSERT_TRUE(win_emu.memory.allocate_memory(base, 0x1000, memory_permission::read_write));
        set_fault_stack_descriptor(base, 0xFFF, 0x40);
        win_emu.emu().reg(x86_register::rsp, 0x1234567800000200ULL);
        win_emu.emu().write_memory<uint64_t>(base + 0x200, 0xDEADBEEF76543210ULL);
        const auto before = win_emu.emu().save_registers();
        win_emu.callbacks.on_memory_violate(0, 4, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        const auto& slot = violations.back().stack_slot;
        EXPECT_EQ(slot.pointer_bits, 32U);
        EXPECT_EQ(slot.address_bits, 32U);
        EXPECT_EQ(slot.segment_base, base);
        EXPECT_EQ(slot.address, base + 0x200U);
        EXPECT_EQ(slot.value, 0x76543210U);
        EXPECT_EQ(slot.width_source, "fault_cs_default_near_return_operand");
        EXPECT_EQ(slot.address_source, "current_descriptor_table");
    }

    TEST_F(AnalysisObservation, CompatibilityStackBZeroUsesSpButSamplesCsDefaultFourBytes)
    {
        constexpr uint32_t base = 0x47000000;
        ASSERT_TRUE(win_emu.memory.allocate_memory(base, 0x1000, memory_permission::read_write));
        set_fault_stack_descriptor(base, 0xFFF, 0);
        win_emu.emu().reg(x86_register::rsp, 0x12345678BEEF0200ULL);
        win_emu.emu().write_memory<uint64_t>(base + 0x200, 0xDEADBEEF12345678ULL);
        const auto before = win_emu.emu().save_registers();
        win_emu.callbacks.on_memory_violate(0, 4, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        const auto& slot = violations.back().stack_slot;
        EXPECT_EQ(slot.pointer_bits, 32U);
        EXPECT_EQ(slot.address_bits, 16U);
        EXPECT_EQ(slot.address, base + 0x200U);
        EXPECT_EQ(slot.value, 0x12345678U);
    }

    TEST_F(AnalysisObservation, StackDescriptorLimitsAndInvalidSelectorsProduceNoSample)
    {
        constexpr uint32_t base = 0x48000000;
        ASSERT_TRUE(win_emu.memory.allocate_memory(base, 0x1000, memory_permission::read_write));
        set_fault_stack_descriptor(base, 0x201, 0x40);
        win_emu.emu().reg(x86_register::rsp, 0x200ULL);
        win_emu.callbacks.on_memory_violate(0, 4, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_FALSE(violations.back().stack_slot.value);
        EXPECT_NE(violations.back().stack_slot.error.find("limit"), std::string::npos);
        set_fault_stack_descriptor(base, 0x100, 0x40, 0xF7);
        win_emu.callbacks.on_memory_violate(0, 4, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_EQ(violations.back().stack_slot.address, base + 0x200U);
        win_emu.emu().reg(x86_register::rsp, 0x100ULL);
        win_emu.callbacks.on_memory_violate(0, 4, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_FALSE(violations.back().stack_slot.value);
        set_fault_stack_descriptor(base, 0xFFFF, 0);
        win_emu.emu().reg(x86_register::rsp, 0xFFFEULL);
        win_emu.callbacks.on_memory_violate(0, 4, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_FALSE(violations.back().stack_slot.value);
        EXPECT_NE(violations.back().stack_slot.error.find("boundary"), std::string::npos);
        win_emu.emu().reg(x86_register::ss, 0);
        win_emu.callbacks.on_memory_violate(0, 4, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_FALSE(violations.back().stack_slot.value);
        EXPECT_FALSE(violations.back().stack_slot.error.empty());
    }

    TEST_F(AnalysisObservation, CompatibilityStackLdtResolutionExcludesMmioReads)
    {
        constexpr uint32_t base = 0x49000000;
        constexpr uint32_t ldt_base = 0x4A000000;
        ASSERT_TRUE(win_emu.memory.allocate_memory(base, 0x1000, memory_permission::read_write));
        ASSERT_TRUE(win_emu.memory.allocate_memory(ldt_base, 0x1000, memory_permission::read_write));
        set_fault_stack_descriptor(base, 0xFFF, 0x40);
        const auto gdt = segment_utils::read_descriptor_table(win_emu.emu(), x86_register::gdtr);
        ASSERT_TRUE(gdt);
        const auto stack_descriptor = win_emu.emu().read_memory<uint64_t>(gdt->base + 0x58);
        win_emu.emu().write_memory<uint64_t>(ldt_base + 8, stack_descriptor);
        const uint64_t ldt_descriptor = 0x0000820000000FFFULL | (static_cast<uint64_t>(ldt_base & 0xFFFF) << 16) |
                                        (static_cast<uint64_t>((ldt_base >> 16) & 0xFF) << 32) |
                                        (static_cast<uint64_t>(ldt_base >> 24) << 56);
        win_emu.emu().write_memory<uint64_t>(gdt->base + 0x60, ldt_descriptor);
        win_emu.emu().write_memory<uint64_t>(gdt->base + 0x68, 0);
        win_emu.emu().reg(x86_register::ldtr, 0x60);
        win_emu.emu().reg(x86_register::ss, 0x0F);
        win_emu.emu().reg(x86_register::rsp, 0x200ULL);
        win_emu.emu().write_memory<uint32_t>(base + 0x200, 0x12345678);
        auto before = win_emu.emu().save_registers();
        win_emu.callbacks.on_memory_violate(0, 4, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(violations.back().stack_slot.address, base + 0x200U);
        EXPECT_EQ(violations.back().stack_slot.value, 0x12345678U);
        ASSERT_TRUE(win_emu.memory.release_memory(ldt_base, 0));
        size_t reads{};
        ASSERT_TRUE(win_emu.memory.allocate_mmio(
            ldt_base, 0x1000, [&](uint64_t, void*, size_t) { ++reads; }, [](uint64_t, const void*, size_t) {}));
        before = win_emu.emu().save_registers();
        win_emu.callbacks.on_memory_violate(0, 4, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(reads, 0U);
        EXPECT_FALSE(violations.back().stack_slot.value);
        EXPECT_FALSE(violations.back().stack_slot.error.empty());
    }

    TEST_F(AnalysisObservation, UnreadableInstructionRetainsFaultAddressAndCpuState)
    {
        const auto address = win_emu.memory.allocate_memory(0x1000, memory_permission::read_write, true);
        ASSERT_NE(address, 0U);
        win_emu.emu().reg(x86_register::rip, address);
        const auto before = win_emu.emu().save_registers();
        win_emu.callbacks.on_memory_violate(address, 1, memory_operation::exec, memory_violation_type::unmapped);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        ASSERT_EQ(violations.size(), 1U);
        const auto& instruction = violations[0].actual_instruction;
        EXPECT_EQ(instruction.location.address, address);
        ASSERT_TRUE(instruction.location.region);
        EXPECT_EQ(instruction.location.region->allocation_base, address);
        EXPECT_FALSE(instruction.location.region->committed);
        EXPECT_TRUE(instruction.bytes_hex.empty());
        EXPECT_TRUE(instruction.assembly.empty());
        EXPECT_EQ(instruction.decoded_size, 0U);
        EXPECT_FALSE(instruction.error.empty());
    }

    TEST_F(AnalysisObservation, DecodeUsesOnlyReadableBytesAtAllocationBoundary)
    {
        const auto address = win_emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        ASSERT_NE(address, 0U);
        ASSERT_TRUE(win_emu.memory.decommit_memory(address + 0x1000, 0x1000));
        win_emu.emu().write_memory<uint8_t>(address + 0xFFF, 0xC3);
        win_emu.emu().reg(x86_register::rip, address + 0xFFF);
        auto before = win_emu.emu().save_registers();
        win_emu.callbacks.on_memory_violate(0, 8, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(violations.back().actual_instruction.bytes_hex, "c3");
        EXPECT_EQ(violations.back().actual_instruction.assembly, "ret");
        EXPECT_EQ(violations.back().actual_instruction.decoded_size, 1U);
        EXPECT_TRUE(violations.back().actual_instruction.error.empty());
        win_emu.emu().write_memory<uint16_t>(address + 0xFFE, 0x8B48);
        win_emu.emu().reg(x86_register::rip, address + 0xFFE);
        before = win_emu.emu().save_registers();
        win_emu.callbacks.on_memory_violate(0, 8, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(violations.back().actual_instruction.bytes_hex, "488b");
        EXPECT_TRUE(violations.back().actual_instruction.assembly.empty());
        EXPECT_EQ(violations.back().actual_instruction.decoded_size, 0U);
        EXPECT_FALSE(violations.back().actual_instruction.error.empty());
    }

    TEST_F(AnalysisObservation, FaultCaptureDoesNotReadMmioOrMutateGuards)
    {
        constexpr uint64_t mmio = 0x42000000;
        size_t reads = 0;
        size_t writes = 0;
        ASSERT_TRUE(win_emu.memory.allocate_mmio(
            mmio, 0x1000, [&](uint64_t, void*, size_t) { ++reads; }, [&](uint64_t, const void*, size_t) { ++writes; }));
        win_emu.emu().reg(x86_register::rip, mmio);
        win_emu.emu().reg(x86_register::rsp, mmio);
        auto before = win_emu.emu().save_registers();
        win_emu.callbacks.on_memory_violate(mmio, 1, memory_operation::exec, memory_violation_type::protection);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(reads, 0U);
        EXPECT_EQ(writes, 0U);
        EXPECT_TRUE(violations.back().actual_instruction.bytes_hex.empty());
        EXPECT_FALSE(violations.back().stack_slot.value);
        const auto guarded = win_emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        ASSERT_NE(guarded, 0U);
        ASSERT_TRUE(win_emu.memory.protect_memory(guarded, 0x1000, memory_permission::read_write | memory_permission_ext::guard));
        win_emu.emu().reg(x86_register::rip, guarded);
        win_emu.emu().reg(x86_register::rsp, guarded);
        before = win_emu.emu().save_registers();
        const auto layout = win_emu.memory.get_layout_version();
        win_emu.callbacks.on_memory_violate(guarded, 1, memory_operation::exec, memory_violation_type::protection);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(win_emu.memory.get_layout_version(), layout);
        EXPECT_TRUE(win_emu.memory.get_region_info(guarded).permissions.is_guarded());
        EXPECT_TRUE(violations.back().actual_instruction.bytes_hex.empty());
        EXPECT_FALSE(violations.back().stack_slot.value);
        ASSERT_TRUE(violations.back().actual_instruction.location.region);
        EXPECT_TRUE(violations.back().actual_instruction.location.region->guarded);
    }

    TEST_F(AnalysisObservation, FaultModeLookupDoesNotReadAnMmioDescriptorTable)
    {
        constexpr uint64_t mmio = 0x43000000;
        size_t reads = 0;
        ASSERT_TRUE(
            win_emu.memory.allocate_mmio(mmio, 0x1000, [&](uint64_t, void*, size_t) { ++reads; }, [](uint64_t, const void*, size_t) {}));
        win_emu.emu().load_gdt(mmio, 0x1000);
        const auto before = win_emu.emu().save_registers();
        win_emu.callbacks.on_memory_violate(0, 4, memory_operation::read, memory_violation_type::unmapped);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(reads, 0U);
        ASSERT_EQ(violations.size(), 1U);
        EXPECT_FALSE(violations[0].code_bits);
        EXPECT_FALSE(violations[0].capture_error.empty());
        EXPECT_FALSE(violations[0].stack_slot.value);
    }

    TEST_F(AnalysisObservation, FaultReportersConsumeOnlyTheSavedSnapshot)
    {
        const std::array<uint8_t, 2> call{0xFF, 0xD0};
        win_emu.emu().write_memory(caller, call.data(), call.size());
        win_emu.current_thread().current_ip = caller;
        win_emu.current_thread().previous_ip = callee;
        win_emu.current_thread().executed_instructions = 2;
        win_emu.emu().reg(x86_register::rip, caller);
        win_emu.emu().write_memory<uint64_t>(stack, 0x1122334455667788ULL);
        win_emu.callbacks.on_memory_violate(0, 8, memory_operation::read, memory_violation_type::unmapped);
        ASSERT_EQ(violations.size(), 1U);
        win_emu.emu().reg(x86_register::rip, callee);
        win_emu.emu().write_memory<uint64_t>(stack, 0);
        const auto before = win_emu.emu().save_registers();
        std::string text;
        std::vector<color> colors;
        logger log;
        log.set_silent(true);
        log.set_sink([&](const color shade, const std::string_view line) {
            text += line;
            colors.push_back(shade);
        });
        auto console = create_console_reporter(log, {});
        console->report(violations[0]);
        EXPECT_NE(text.find("Stack slot64"), std::string::npos);
        EXPECT_NE(text.find("0x1122334455667788"), std::string::npos);
        EXPECT_NE(text.find("Last tracked (fault CS)"), std::string::npos);
        EXPECT_EQ(text.find("near-null execute"), std::string::npos);
        EXPECT_EQ(text.find("return address"), std::string::npos);
        EXPECT_EQ(text.find("Null-pointer call"), std::string::npos);
        EXPECT_NE(std::ranges::find(colors, color::red), colors.end());
        const auto file = std::filesystem::temp_directory_path() /
                          ("sogen-fault-observation-" + std::to_string(getpid()) + "-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");
        const auto cleanup = utils::finally([&] {
            std::error_code error;
            std::filesystem::remove(file, error);
        });
        auto jsonl = create_jsonl_reporter(file);
        jsonl->report(violations[0]);
        jsonl->flush();
        const auto bytes = utils::io::read_file(file);
        const std::string json(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        EXPECT_NE(json.find("\"nearNullExecute\":false"), std::string::npos);
        EXPECT_NE(json.find("\"actualInstruction\":"), std::string::npos);
        EXPECT_NE(json.find("\"lastTrackedInstruction\":"), std::string::npos);
        EXPECT_NE(json.find("\"bytes\":\"ffd0\""), std::string::npos);
        EXPECT_NE(json.find("\"asm\":\"call rax\""), std::string::npos);
        EXPECT_NE(json.find("\"moduleBase\":"), std::string::npos);
        EXPECT_NE(json.find("\"moduleRva\":"), std::string::npos);
        EXPECT_NE(json.find("\"allocationBase\":"), std::string::npos);
        EXPECT_NE(json.find("\"pointerBits\":64"), std::string::npos);
        EXPECT_NE(json.find("\"addressBits\":64"), std::string::npos);
        EXPECT_NE(json.find("\"widthSource\":\"fault_cs_default_near_return_operand\""), std::string::npos);
        EXPECT_NE(json.find("\"addressSource\":\"64_bit_rsp\""), std::string::npos);
        EXPECT_NE(json.find("\"value\":\"0x1122334455667788\""), std::string::npos);
        EXPECT_NE(json.find("\"prev\":"), std::string::npos);
        EXPECT_EQ(json.find("returnAddress"), std::string::npos);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
    }

    TEST_F(AnalysisObservation, FunctionDetailsPreserveGuestRegistersAndReturnSlot)
    {
        const auto before = win_emu.emu().save_registers();
        win_emu.callbacks.on_instruction(callee);
        EXPECT_EQ(win_emu.emu().save_registers(), before);
        EXPECT_EQ(win_emu.emu().read_memory<uint64_t>(stack), caller + 2);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_EQ(calls.front().function_name, "WinVerifyTrust");
        EXPECT_EQ(calls.front().execution.rip, callee);
    }

    TEST_F(AnalysisObservation, TracedCallReturnsWithGuestResultAndBalancedStack)
    {
        const std::array<uint8_t, 3> call{0xFF, 0xD0, 0x90};
        const std::array<uint8_t, 6> body{0xB8, 0x00, 0x01, 0x0B, 0x80, 0xC3};
        win_emu.emu().write_memory(caller, call.data(), call.size());
        win_emu.emu().write_memory(callee, body.data(), body.size());
        win_emu.emu().write_memory<uint64_t>(stack, caller + 0x80);
        win_emu.emu().reg(x86_register::rip, caller);
        win_emu.emu().reg(x86_register::rax, callee);
        win_emu.emu().start(3);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rip), caller + 2);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rsp), stack);
        EXPECT_EQ(win_emu.emu().reg<uint64_t>(x86_register::rax), 0x800B0100U);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_EQ(calls.front().execution.rip, callee);
    }

    TEST_F(AnalysisObservation, VerboseIncludesUnselectedModuleCalls)
    {
        const auto target = win_emu.mod_manager.ntdll->image_base + 0x500;
        win_emu.mod_manager.ntdll->address_names[target] = "ObservedFunction";
        observe(target + 0x40, target);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_FALSE(calls.front().interesting);
        EXPECT_EQ(calls.front().function_name, "ObservedFunction");
        calls.clear();
        logging_settings.verbose_logging = false;
        observe(target + 0x40, target);
        EXPECT_TRUE(calls.empty());
    }

    TEST_F(AnalysisObservation, SelectedModulesRetainIncomingAndOutgoingCalls)
    {
        logging_settings.verbose_logging = false;
        const auto target = win_emu.mod_manager.ntdll->image_base + 0x500;
        win_emu.mod_manager.ntdll->address_names[target] = "ObservedFunction";
        logging_settings.modules.insert(win_emu.mod_manager.ntdll->name);
        observe(target + 0x40, target);
        observe(target + 0x40, callee);
        observe(caller, target);
        ASSERT_EQ(calls.size(), 3U);
        for (const auto& call : calls)
        {
            EXPECT_TRUE(call.interesting);
        }
    }

    TEST_F(AnalysisObservation, AnonymousCallerIsInteresting)
    {
        logging_settings.verbose_logging = false;
        const auto target = win_emu.mod_manager.ntdll->image_base + 0x500;
        win_emu.mod_manager.ntdll->address_names[target] = "ObservedFunction";
        ASSERT_EQ(win_emu.mod_manager.find_by_address(stack), nullptr);
        observe(stack, target);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_TRUE(calls.front().interesting);
    }

    TEST_F(AnalysisObservation, EntryPointPreservesCallClassification)
    {
        auto& module = *win_emu.mod_manager.ntdll;
        const auto target = module.image_base + 0x500;
        module.entry_point = target;
        module.address_names.erase(target);
        observe(target + 0x40, target);
        observe(caller, target);
        ASSERT_EQ(entries.size(), 2U);
        EXPECT_FALSE(entries.front().interesting);
        EXPECT_TRUE(entries.back().interesting);
        EXPECT_TRUE(calls.empty());
    }

    TEST_F(AnalysisObservation, ForeignTransitionDistinguishesBranchFromReturn)
    {
        const auto target = win_emu.mod_manager.ntdll->image_base + 0x500;
        win_emu.mod_manager.ntdll->address_names[target] = "ObservedFunction";
        win_emu.mod_manager.ntdll->address_names.erase(target + 3);
        const std::array<uint8_t, 5> branch{0xE9, 0, 0, 0, 0};
        win_emu.emu().write_memory(caller, branch.data(), branch.size());
        observe(caller, target + 3);
        ASSERT_EQ(transitions.size(), 1U);
        EXPECT_TRUE(transitions.front().interesting);
        EXPECT_EQ(transitions.front().function_name, "ObservedFunction");
        EXPECT_EQ(transitions.front().function_offset, 3U);
        win_emu.emu().write_memory<uint8_t>(caller, 0xC3);
        observe(caller, target + 3);
        EXPECT_EQ(transitions.size(), 1U);
    }

    TEST_F(AnalysisObservation, ExportBoundsKeepNamedCallsAndForeignTransitions)
    {
        auto& module = *win_emu.mod_manager.ntdll;
        const auto first = module.image_base + 0x500;
        const auto last = first + 0x100;
        module.address_names.clear();
        module.address_names[first] = "FirstExport";
        module.address_names[last] = "LastExport";
        const auto same_module_caller = last + 0x100;
        observe(same_module_caller, first - 1);
        observe(same_module_caller, last + 1);
        observe(same_module_caller, first + 1);
        EXPECT_TRUE(calls.empty());
        observe(same_module_caller, first);
        observe(same_module_caller, last);
        ASSERT_EQ(calls.size(), 2U);
        EXPECT_EQ(calls[0].function_name, "FirstExport");
        EXPECT_EQ(calls[1].function_name, "LastExport");
        const std::array<uint8_t, 5> branch{0xE9, 0, 0, 0, 0};
        win_emu.emu().write_memory(caller, branch.data(), branch.size());
        observe(caller, last + 3);
        ASSERT_EQ(transitions.size(), 1U);
        EXPECT_EQ(transitions[0].function_name, "LastExport");
        EXPECT_EQ(transitions[0].function_offset, 3U);
    }

    TEST_F(AnalysisObservation, IgnoredFunctionsRemainAbsentInVerboseMode)
    {
        logging_settings.ignored_functions.insert("WinVerifyTrust");
        observe(caller, callee);
        EXPECT_TRUE(calls.empty());
    }

    TEST_F(AnalysisObservation, UnnamedInstructionsStillContributeToSelectedSummaries)
    {
        auto& module = *win_emu.mod_manager.ntdll;
        const auto target = module.image_base + 0x500;
        module.address_names.erase(target);
        win_emu.emu().write_memory<uint8_t>(target, 0x90);
        logging_settings.instruction_summary = true;
        observe(target + 0x40, target);
        EXPECT_TRUE(analysis.instructions.empty());
        logging_settings.modules.insert(module.name);
        observe(target + 0x40, target);
        ASSERT_EQ(analysis.instructions.size(), 1U);
        EXPECT_EQ(analysis.instructions.begin()->second, 1U);
        EXPECT_TRUE(calls.empty());
        EXPECT_TRUE(entries.empty());
        EXPECT_TRUE(transitions.empty());
    }

    TEST_F(AnalysisObservation, FirstAnonymousInstructionDoesNotMakeAnUnselectedCallInteresting)
    {
        const auto target = win_emu.mod_manager.ntdll->image_base + 0x500;
        win_emu.mod_manager.ntdll->address_names[target] = "ObservedFunction";
        auto& thread = win_emu.current_thread();
        thread.previous_ip = stack;
        thread.executed_instructions = 1;
        win_emu.emu().reg(x86_register::rip, target);
        win_emu.callbacks.on_instruction(target);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_FALSE(calls.front().interesting);
        calls.clear();
        logging_settings.verbose_logging = false;
        win_emu.callbacks.on_instruction(target);
        EXPECT_TRUE(calls.empty());
    }

    TEST_F(AnalysisObservation, ThreadExitReportsTargetAndFullStatusWithoutExitingProcess)
    {
        for (const auto status : std::array<NTSTATUS, 2>{STATUS_SUCCESS, STATUS_ACCESS_VIOLATION})
        {
            const auto target = win_emu.process.create_thread(win_emu.memory, caller, 0, 0x10000, 0);
            const auto id = win_emu.process.threads.get(target)->id;
            auto& vcpu = win_emu.vcpu(0);
            const syscall_context context{.win_emu = win_emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = win_emu.process};
            ASSERT_NE(id, vcpu.active_thread->id);
            ASSERT_EQ(syscalls::handle_NtTerminateThread(context, target, status), STATUS_SUCCESS);
            ASSERT_FALSE(terminated.empty());
            EXPECT_EQ(terminated.back().terminated_thread_id, id);
            EXPECT_EQ(terminated.back().exit_status, static_cast<uint32_t>(status));
            EXPECT_FALSE(vcpu.active_thread->exit_status.has_value());
            EXPECT_FALSE(win_emu.process.exit_status.has_value());
        }
        EXPECT_EQ(terminated.size(), 2U);
    }

    TEST_F(AnalysisObservation, UnnamedInstructionPrunesOnlyTheCurrentDebugPrintStack)
    {
        const auto id = win_emu.current_thread().id;
        analysis.debug_print_calls[id].push_back({.call_id = 7, .stack = stack, .return_address = caller});
        analysis.debug_print_calls[id + 4].push_back({.call_id = 8, .stack = stack, .return_address = caller});
        observe(caller - 1, caller);
        EXPECT_FALSE(analysis.debug_print_calls.contains(id));
        ASSERT_TRUE(analysis.debug_print_calls.contains(id + 4));
        EXPECT_EQ(analysis.debug_print_calls.at(id + 4).front().call_id, 8U);
        EXPECT_TRUE(calls.empty());
    }

    TEST_F(AnalysisObservation, DeferredImportReadKeepsItsInstructionBoundary)
    {
        const auto id = win_emu.current_thread().id;
        analysis.accessed_imports.push_back({.address = callee,
                                             .access_context = {.thread_id = id},
                                             .access_inst_count = 100,
                                             .import_name = "DeferredImport",
                                             .import_module = "test.dll"});
        win_emu.current_thread().executed_instructions = 199;
        win_emu.callbacks.on_instruction(caller);
        EXPECT_TRUE(imports.empty());
        ASSERT_EQ(analysis.accessed_imports.size(), 1U);
        win_emu.current_thread().executed_instructions = 200;
        win_emu.callbacks.on_instruction(caller);
        ASSERT_EQ(imports.size(), 1U);
        EXPECT_EQ(imports.front().resolved_address, callee);
        EXPECT_EQ(imports.front().import_name, "DeferredImport");
        EXPECT_TRUE(analysis.accessed_imports.empty());
        win_emu.callbacks.on_instruction(caller);
        EXPECT_EQ(imports.size(), 1U);
    }

    TEST_F(AnalysisObservation, ProgressKeepsModeAndInstructionGates)
    {
        analysis.progress_last -= std::chrono::seconds(6);
        logging_settings.verbose_logging = false;
        win_emu.callbacks.on_instruction(caller);
        EXPECT_TRUE(progress.empty());
        logging_settings.verbose_logging = true;
        logging_settings.reproducible = true;
        win_emu.callbacks.on_instruction(caller);
        EXPECT_TRUE(progress.empty());
        logging_settings.reproducible = false;
        win_emu.emu().write_memory<uint8_t>(caller, 0x90);
        win_emu.emu().reg(x86_register::rip, caller);
        win_emu.emu().start(1);
        ASSERT_EQ(win_emu.get_executed_instructions(), 1U);
        EXPECT_TRUE(progress.empty());
        win_emu.callbacks.on_instruction(caller);
        EXPECT_TRUE(progress.empty());
    }

    TEST_F(AnalysisObservation, ProgressExcludesInstructionsFromRestoredSnapshot)
    {
        const std::array<uint8_t, 2> loop{0xEB, 0xFE};
        win_emu.emu().write_memory(caller, loop.data(), loop.size());
        win_emu.emu().reg(x86_register::rip, caller);
        win_emu.emu().start(0x20000);
        win_emu.emu().start(0x20000);
        const auto count = win_emu.get_executed_instructions();
        ASSERT_EQ(count, 0x40000U);
        utils::buffer_serializer output{};
        win_emu.serialize(output);
        utils::buffer_deserializer input{output.get_buffer()};
        win_emu.deserialize(input);
        analysis_context restored{.settings = &logging_settings, .win_emu = &win_emu, .reporters = {this}};
        register_analysis_callbacks(restored);
        restored.progress_last -= std::chrono::seconds(6);
        progress.clear();
        win_emu.callbacks.on_instruction(caller);
        ASSERT_EQ(progress.size(), 1U);
        EXPECT_EQ(progress.front().header.instruction_count, count);
        EXPECT_EQ(progress.front().instructions_per_second, 0U);
    }

}
