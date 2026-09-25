#include "emulation_test_utils.hpp"
#include "../windows-analyzer/analysis.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"
#include "../windows-analyzer/jsonl_reporter.hpp"
#include "../windows-analyzer/debug_print.hpp"
#include <emulator_utils.hpp>
#include <syscall_utils.hpp>
#include <utils/finally.hpp>
#include <utils/io.hpp>

namespace sogen::syscalls
{
    NTSTATUS handle_NtSetEvent(const syscall_context&, uint64_t, emulator_object<LONG>);
}

namespace sogen::test
{
    class DebugPrintCapture : public testing::Test, public analysis_reporter
    {
      protected:
        windows_emulator win = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        analysis_settings logging_settings{};
        analysis_context context{.settings = &logging_settings, .win_emu = &win, .reporters = {this}};
        std::vector<debug_print_call_event> calls;
        std::vector<debug_string_event> output;
        static constexpr uint64_t data = 0x30000000;
        static constexpr uint64_t stack = data + 0x8000;
        uint64_t entry{};

        void report(const analysis_event& event) override
        {
            if (const auto* call = std::get_if<debug_print_call_event>(&event))
            {
                calls.push_back(*call);
            }
            if (const auto* print = std::get_if<debug_string_event>(&event))
            {
                output.push_back(*print);
            }
        }

        void SetUp() override
        {
            win.setup_process_if_necessary();
            ASSERT_TRUE(win.memory.allocate_memory(data, 0x10000, memory_permission::read_write));
            entry = win.mod_manager.executable->entry_point;
            win.emu().reg(x86_register::rsp, stack);
            win.emu().reg(x86_register::rip, entry);
            win.emu().write_memory<uint64_t>(stack, entry + 0x90);
            register_analysis_callbacks(context);
        }

        void string(const std::string_view value, const uint64_t address = data)
        {
            win.memory.write_memory(address, value.data(), value.size());
            win.emu().write_memory<uint8_t>(address + value.size(), 0);
        }

        void wide(const std::u16string_view value, const uint64_t address = data)
        {
            win.memory.write_memory(address, value.data(), value.size() * 2);
            win.emu().write_memory<uint16_t>(address + value.size() * 2, 0);
        }

        void argument(const size_t index, const uint64_t value, const bool bits32 = false)
        {
            if (bits32)
            {
                win.emu().write_memory<uint32_t>(stack + 4 + index * 4, static_cast<uint32_t>(value));
            }
            else if (index < 4)
            {
                constexpr std::array regs{x86_register::rcx, x86_register::rdx, x86_register::r8, x86_register::r9};
                win.emu().reg(regs[index], value);
            }
            else
            {
                win.emu().write_memory<uint64_t>(stack + 8 + index * 8, value);
            }
        }
    };

    TEST_F(DebugPrintCapture, AnsiAndUnicodeArgumentsPreserveGuestState)
    {
        string("hello %s\n");
        argument(0, data);
        const auto registers = win.emu().save_registers();
        observe_debug_print_call(context, "OutputDebugStringA");
        EXPECT_EQ(registers, win.emu().save_registers());
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_EQ(calls[0].arguments[0].text, "hello %s\n");
        EXPECT_EQ(calls[0].return_address, entry + 0x90);
        EXPECT_EQ(calls[0].pointer_bits, 64U);
        wide(u"Dawn \u03a9 \U0001f600");
        observe_debug_print_call(context, "OutputDebugStringW");
        EXPECT_EQ(calls.back().arguments[0].bytes_hex, "4400610077006e002000a90320003dd800de");
        EXPECT_EQ(calls.back().arguments[0].encoding, "utf-16le");
    }

    TEST_F(DebugPrintCapture, VariadicFixedArgumentsAndStringsAreCaptured)
    {
        string("id=%08x s=%s wide=%ws value=%I64x");
        string("argument", data + 0x100);
        wide(u"wide", data + 0x200);
        argument(0, 77);
        argument(1, 4);
        argument(2, data);
        argument(3, 0x1234);
        argument(4, data + 0x100);
        argument(5, data + 0x200);
        argument(6, 0x1122334455667788ULL);
        observe_debug_print_call(context, "DbgPrintEx");
        ASSERT_EQ(calls.size(), 1U);
        const auto& args = calls[0].arguments;
        ASSERT_EQ(args.size(), 7U);
        EXPECT_EQ(args[3].raw, 0x1234U);
        EXPECT_EQ(args[4].text, "argument");
        EXPECT_EQ(args[5].text, "wide");
        EXPECT_EQ(args[6].raw, 0x1122334455667788ULL);
    }

    TEST_F(DebugPrintCapture, X86StackAndDoubleWordArgumentsUseFourByteSlots)
    {
        win.emu().reg(x86_register::cs, 0x23);
        ASSERT_TRUE(is_32bit_code_segment(win.emu()));
        string("%I64x %s %d");
        string("x86", data + 0x100);
        argument(0, data, true);
        argument(1, 0x55667788, true);
        argument(2, 0x11223344, true);
        argument(3, data + 0x100, true);
        argument(4, 42, true);
        observe_debug_print_call(context, "_DbgPrint");
        ASSERT_EQ(calls.size(), 1U);
        ASSERT_EQ(calls[0].arguments.size(), 4U);
        EXPECT_EQ(calls[0].pointer_bits, 32U);
        EXPECT_EQ(calls[0].arguments[1].raw, 0x1122334455667788ULL);
        EXPECT_EQ(calls[0].arguments[2].text, "x86");
        EXPECT_EQ(calls[0].arguments[3].raw, 42U);
    }

    TEST_F(DebugPrintCapture, VaListWithPrefixIsCapturedWithoutExecutingFormat)
    {
        string("prefix");
        string("%*.*s %% %n", data + 0x100);
        string("abc", data + 0x200);
        win.emu().write_memory<uint64_t>(data + 0x400, 9);
        win.emu().write_memory<uint64_t>(data + 0x408, 2);
        win.emu().write_memory<uint64_t>(data + 0x410, data + 0x200);
        win.emu().write_memory<uint64_t>(data + 0x418, data + 0x500);
        win.emu().write_memory<uint32_t>(data + 0x500, 0x12345678);
        argument(0, data);
        argument(1, 77);
        argument(2, 1);
        argument(3, data + 0x100);
        argument(4, data + 0x400);
        observe_debug_print_call(context, "vDbgPrintExWithPrefix");
        ASSERT_EQ(calls.size(), 1U);
        ASSERT_EQ(calls[0].arguments.size(), 9U);
        EXPECT_EQ(calls[0].arguments[7].text, "ab");
        EXPECT_EQ(win.memory.read_memory<uint32_t>(data + 0x500), 0x12345678U);
        EXPECT_TRUE(output.empty());
    }

    TEST_F(DebugPrintCapture, CountedStringPreservesEmbeddedNul)
    {
        string("%wZ");
        wide(u"a\0b"sv, data + 0x200);
        win.emu().write_memory<uint16_t>(data + 0x100, 6);
        win.emu().write_memory<uint16_t>(data + 0x102, 8);
        win.emu().write_memory<uint64_t>(data + 0x108, data + 0x200);
        argument(0, data);
        argument(1, data + 0x100);
        observe_debug_print_call(context, "DbgPrint");
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_EQ(calls[0].arguments[1].text, "a\0b"s);
        EXPECT_EQ(calls[0].arguments[1].raw, data + 0x100);
    }

    TEST_F(DebugPrintCapture, UnicodeExceptionAndAnsiFallbackAreSeparateEmissions)
    {
        wide(u"original \u03a9");
        argument(0, data);
        observe_debug_print_call(context, "OutputDebugStringW");
        const auto call_id = calls[0].call_id;
        const auto record = data + 0x200;
        win.emu().write_memory<uint32_t>(record, 0x4001000a);
        win.emu().write_memory<uint32_t>(record + 24, 4);
        win.emu().write_memory<uint64_t>(record + 32, 11);
        win.emu().write_memory<uint64_t>(record + 40, data);
        win.emu().write_memory<uint64_t>(record + 48, 11);
        win.emu().write_memory<uint64_t>(record + 56, data + 0x100);
        string("original ?", data + 0x100);
        argument(0, record);
        observe_debug_print_call(context, "RtlRaiseException");
        ASSERT_EQ(output.size(), 1U);
        EXPECT_EQ(output[0].transport, "debug_exception");
        EXPECT_EQ(output[0].encoding, "utf-16le");
        ASSERT_TRUE(output[0].ansi_fallback.has_value());
        EXPECT_EQ(output[0].ansi_fallback->text, "original ?");

        EXPECT_EQ(output[0].origin_calls, std::vector<uint64_t>{call_id});
    }

    TEST_F(DebugPrintCapture, Int2dCapturesCountedBytesAndDoesNotNotifyGdbOutput)
    {
        string("a\0b"sv);
        size_t gdb_messages = 0;
        win.callbacks.on_debug_string.add([&](std::string_view) { ++gdb_messages; });
        observe_debug_print_interrupt(context, data, 3, 77, 3);
        ASSERT_EQ(output.size(), 1U);
        EXPECT_EQ(output[0].details, "a\0b"s);
        EXPECT_EQ(output[0].bytes_hex, "610062");
        EXPECT_EQ(output[0].component, 77U);
        EXPECT_EQ(gdb_messages, 0U);
    }

    TEST_F(DebugPrintCapture, ReturnedCallDoesNotOwnLaterOutput)
    {
        string("done");
        argument(0, data);
        observe_debug_print_call(context, "OutputDebugStringA");
        prune_debug_print_calls(context, entry + 0x90);
        observe_debug_string(context, "later");
        ASSERT_EQ(output.size(), 1U);
        EXPECT_TRUE(output[0].origin_calls.empty());
    }

    TEST_F(DebugPrintCapture, NullAndUnreadablePointersDoNotChangeTheGuest)
    {
        argument(0, 0);
        observe_debug_print_call(context, "OutputDebugStringA");
        argument(0, 0xdead0000);
        observe_debug_print_call(context, "OutputDebugStringW");
        ASSERT_EQ(calls.size(), 2U);
        EXPECT_EQ(calls[0].arguments[0].error, "null pointer");
        EXPECT_EQ(calls[1].arguments[0].error, "unreadable memory");
        EXPECT_FALSE(win.process.exit_status.has_value());
    }

    TEST_F(DebugPrintCapture, DebugCallsAreCapturedEvenWithoutVerboseLogging)
    {
        string("always");
        argument(0, data);
        win.mod_manager.executable->address_names[entry] = "OutputDebugStringA";
        win.callbacks.on_instruction(entry);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_EQ(calls[0].arguments[0].text, "always");
    }

    TEST_F(DebugPrintCapture, ConsoleEscapesControlCharacters)
    {
        EXPECT_EQ(escape_debug_console("a\x1b[31m\n\r\t\0z"sv), "a\\x1B[31m\\n\\r\\t\\x00z");
    }

    TEST_F(DebugPrintCapture, X86WideCountPointerRetainsPointerWidth)
    {
        win.emu().reg(x86_register::cs, 0x23);
        ASSERT_TRUE(is_32bit_code_segment(win.emu()));
        string("%I64n %s");
        string("next", data + 0x100);
        argument(0, data, true);
        argument(1, data + 0x200, true);
        argument(2, data + 0x100, true);
        observe_debug_print_call(context, "DbgPrint");
        ASSERT_EQ(calls.size(), 1U);
        ASSERT_EQ(calls[0].arguments.size(), 3U);
        EXPECT_EQ(calls[0].arguments[1].raw, data + 0x200);
        EXPECT_EQ(calls[0].arguments[2].text, "next");
    }

    TEST_F(DebugPrintCapture, FailedVaListReadStillAdvancesPastEightByteOperand)
    {
        win.emu().reg(x86_register::cs, 0x23);
        string("%I64x %d");
        win.emu().write_memory<uint32_t>(data + 0xfffc, 0x1234);
        argument(0, 77, true);
        argument(1, 1, true);
        argument(2, data, true);
        argument(3, data + 0xfffc, true);
        observe_debug_print_call(context, "vDbgPrintEx");
        ASSERT_EQ(calls.size(), 1U);
        ASSERT_EQ(calls[0].arguments.size(), 6U);
        EXPECT_FALSE(calls[0].arguments[4].error.empty());
        EXPECT_FALSE(calls[0].arguments[5].error.empty());
        EXPECT_EQ(calls[0].arguments[5].raw, 0U);
    }

    TEST_F(DebugPrintCapture, X86DebuggerCallUsesMatchedTransportArguments)
    {
        win.emu().reg(x86_register::cs, 0x23);
        string("x86\0output"sv);
        argument(0, 1, true);
        argument(1, data, true);
        argument(2, 10, true);
        argument(3, 77, true);
        argument(4, 5, true);
        observe_debug_print_call(context, "_NtWow64DebuggerCall@20");
        ASSERT_EQ(output.size(), 1U);
        EXPECT_EQ(output[0].transport, "wow64_debugger_call");
        EXPECT_EQ(output[0].details, "x86\0output"s);
        EXPECT_EQ(output[0].component, 77U);
        EXPECT_EQ(output[0].level, 5U);
    }

    TEST_F(DebugPrintCapture, PassiveReadsDoNotInvokeMmioCallbacks)
    {
        size_t reads = 0;
        ASSERT_TRUE(win.memory.allocate_mmio(
            data + 0x10000, 0x1000, [&](uint64_t, void*, size_t) { ++reads; }, [](uint64_t, const void*, size_t) {}));
        argument(0, data + 0x10000);
        observe_debug_print_call(context, "OutputDebugStringW");
        string("%Z");
        argument(0, data);
        argument(1, data + 0x10000);
        observe_debug_print_call(context, "DbgPrint");
        argument(0, data + 0x10000);
        observe_debug_print_call(context, "RtlRaiseException");
        EXPECT_EQ(reads, 0U);
        EXPECT_FALSE(calls[0].arguments[0].error.empty());
        EXPECT_FALSE(calls[1].arguments[1].error.empty());
    }

    TEST_F(DebugPrintCapture, OddAlignedUnicodeDoesNotReadAcrossIntoMmio)
    {
        size_t reads = 0;
        ASSERT_TRUE(win.memory.allocate_mmio(
            data + 0x10000, 0x1000, [&](uint64_t, void*, size_t) { ++reads; }, [](uint64_t, const void*, size_t) {}));
        win.emu().write_memory<uint8_t>(data + 0xffff, 0x61);
        argument(0, data + 0xffff);
        observe_debug_print_call(context, "OutputDebugStringW");
        EXPECT_EQ(reads, 0U);
        ASSERT_EQ(calls.size(), 1U);
        EXPECT_EQ(calls[0].arguments[0].error, "unreadable memory");
    }

    TEST_F(DebugPrintCapture, OodleErrorCapturesBoundedCpuStateWithoutChangingNormalPrints)
    {
        win.emu().reg(x86_register::rax, 0x12345678);
        win.emu().write_memory<uint64_t>(stack + 8, entry + 0x188);
        observe_debug_string(context, "normal print\n");
        ASSERT_EQ(output.size(), 1U);
        EXPECT_FALSE(output.back().cpu_snapshot.has_value());
        observe_debug_string(context, "OODLE ERROR : LZ corruption : bad decode len\n");
        ASSERT_EQ(output.size(), 2U);
        ASSERT_TRUE(output.back().cpu_snapshot.has_value());
        const auto& snapshot = *output.back().cpu_snapshot;
        EXPECT_EQ(snapshot.pointer_bits, 64U);
        EXPECT_EQ(snapshot.instruction_pointer, entry);
        EXPECT_EQ(snapshot.stack_pointer, stack);
        EXPECT_EQ(snapshot.gprs[0], 0x12345678U);
        ASSERT_GE(snapshot.stack_words.size(), 2U);
        EXPECT_EQ(snapshot.stack_words[0], entry + 0x90);
        EXPECT_EQ(snapshot.stack_words[1], entry + 0x188);
        EXPECT_LE(snapshot.stack_words.size(), 64U);

        const auto file = std::filesystem::temp_directory_path() / ("sogen-oodle-print-" + std::to_string(getpid()) + ".jsonl");
        const auto cleanup = utils::finally([&] {
            std::error_code error;
            std::filesystem::remove(file, error);
        });
        auto reporter = create_jsonl_reporter(file);
        reporter->report(output.back());
        reporter->flush();
        const auto saved = utils::io::read_file(file);
        const std::string json(reinterpret_cast<const char*>(saved.data()), saved.size());
        EXPECT_NE(json.find("\"cpu_snapshot\":"), std::string::npos);
        EXPECT_NE(json.find("\"pointer_bits\":64"), std::string::npos);
        EXPECT_NE(json.find("\"stack_words\":[\"0x"), std::string::npos);
    }

    TEST_F(DebugPrintCapture, X86OodleErrorCapturesFourByteStackWords)
    {
        win.emu().reg(x86_register::cs, 0x23);
        win.emu().reg(x86_register::esp, static_cast<uint32_t>(stack));
        win.emu().write_memory<uint32_t>(stack, 0x10203040);
        win.emu().write_memory<uint32_t>(stack + 4, 0x50607080);
        observe_debug_string(context, "OODLE ERROR : LZ corruption : not enough comp buf\n");
        ASSERT_EQ(output.size(), 1U);
        ASSERT_TRUE(output.back().cpu_snapshot.has_value());
        const auto& snapshot = *output.back().cpu_snapshot;
        EXPECT_EQ(snapshot.pointer_bits, 32U);
        EXPECT_EQ(snapshot.stack_pointer, stack);
        ASSERT_GE(snapshot.stack_words.size(), 2U);
        EXPECT_EQ(snapshot.stack_words[0], 0x10203040U);
        EXPECT_EQ(snapshot.stack_words[1], 0x50607080U);
        EXPECT_EQ(snapshot.gprs[8], 0U);
    }

    TEST_F(DebugPrintCapture, DbwinIsBoundedAndCaptureFailurePreservesSuccess)
    {
        win.process.dbwin_buffer = data + 0xf000;
        std::array<char, 4092> bytes{};
        bytes.fill('x');
        win.memory.write_memory(win.process.dbwin_buffer + 4, bytes.data(), bytes.size());
        syscall_context syscall{.win_emu = win, .emu = win.emu(), .vcpu = win.vcpu(0), .proc = win.process};
        EXPECT_EQ(syscalls::handle_NtSetEvent(syscall, DBWIN_DATA_READY.bits, {win.memory, 0}), STATUS_SUCCESS);
        ASSERT_EQ(output.size(), 1U);
        EXPECT_EQ(output[0].details.size(), bytes.size());
        win.process.dbwin_buffer = 0xdead0000;
        EXPECT_EQ(syscalls::handle_NtSetEvent(syscall, DBWIN_DATA_READY.bits, {win.memory, 0}), STATUS_SUCCESS);
        ASSERT_EQ(output.size(), 2U);
        EXPECT_EQ(output[1].error, "DBWIN buffer unreadable");
        EXPECT_TRUE(output[1].details.empty());
    }

    TEST_F(DebugPrintCapture, ActualInt2dHookPreservesRegistersAndSkipsTrapByte)
    {
        string("int2d");
        const std::array<uint8_t, 4> code{0xcd, 0x2d, 0xcc, 0x90};
        win.emu().write_memory(entry, code.data(), code.size());
        win.emu().reg(x86_register::rax, 1);
        win.emu().reg(x86_register::rcx, data);
        win.emu().reg(x86_register::rdx, 5);
        win.emu().reg(x86_register::r8, 77);
        win.emu().reg(x86_register::r9, 3);
        win.emu().start(1);
        ASSERT_EQ(output.size(), 1U);
        EXPECT_EQ(output[0].details, "int2d");
        EXPECT_EQ(win.emu().reg<uint64_t>(x86_register::rip), entry + 3);
        EXPECT_EQ(win.emu().reg<uint64_t>(x86_register::rax), 1U);
        EXPECT_FALSE(win.process.exit_status.has_value());
    }
}
