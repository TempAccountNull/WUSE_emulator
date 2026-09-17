#include "std_include.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"
#include "../windows-analyzer/jsonl_reporter.hpp"

#include <gtest/gtest.h>
#include <logger.hpp>
#include <utils/async_file_writer.hpp>
#include <utils/finally.hpp>
#include <utils/io.hpp>

#include <atomic>
#include <fstream>
#include <thread>

namespace sogen
{
    namespace
    {
        std::filesystem::path unique_path(const char* stem, const char* extension)
        {
            static std::atomic_uint64_t sequence{};
            return std::filesystem::temp_directory_path() /
                   (std::string(stem) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                    std::to_string(sequence.fetch_add(1)) + extension);
        }

        function_execution_event function_call(const uint32_t tid, const uint64_t call_count, const std::string& name,
                                               const bool interesting = true, const uint64_t previous_ip = 0x14187e85c)
        {
            function_execution_event event{};
            event.execution.thread_id = tid;
            event.execution.rip = 0x15003bc3f0;
            event.execution.rip_module = "vcruntime140.dll";
            event.execution.previous_ip = previous_ip;
            event.execution.previous_ip_module = "destiny2.exe";
            event.call_count = call_count;
            event.function_name = name;
            event.interesting = interesting;
            return event;
        }

        size_t count_lines(const std::string& text, const std::string& needle)
        {
            size_t count = 0;
            for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
            {
                ++count;
            }
            return count;
        }

        // Lines that begin with `prefix`; repeat summaries quote the original line, so a plain
        // substring count would include them.
        size_t count_prefixed(const std::string& text, const std::string& prefix)
        {
            size_t count = 0;
            size_t start = 0;
            while (start < text.size())
            {
                const auto end = text.find('\n', start);
                const auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                count += line.starts_with(prefix) ? 1 : 0;
                if (end == std::string::npos)
                {
                    break;
                }
                start = end + 1;
            }
            return count;
        }

        std::string read_text(const std::filesystem::path& path)
        {
            const auto bytes = utils::io::read_file(path);
            return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        }

        struct captured_console
        {
            std::string text;
            logger log;
            std::unique_ptr<analysis_reporter> console;

            explicit captured_console(console_reporter_settings settings)
            {
                this->log.set_silent(true);
                this->log.set_sink([this](const color, const std::string_view line) { this->text += line; });
                this->console = create_console_reporter(this->log, settings);
            }
        };
    }

    // ------------------------------------------------------------------ console repeat coalescing

    TEST(ConsoleCoalescing, DisabledByDefaultPrintsEveryLine)
    {
        captured_console fixture({.interesting_only = true});
        for (uint64_t i = 1; i <= 3; ++i)
        {
            fixture.console->report(function_call(8, i, "memcpy"));
        }
        EXPECT_EQ(count_lines(fixture.text, "Executing function: memcpy"), 3U);
    }

    TEST(ConsoleCoalescing, FirstEventPrintsAndIdenticalRepeatsAreAbsorbed)
    {
        captured_console fixture({.interesting_only = true, .coalesce_repeats = true, .repeat_summary_every = 1000});
        for (uint64_t i = 1; i <= 10; ++i)
        {
            fixture.console->report(function_call(8, i, "memcpy"));
        }
        EXPECT_EQ(count_lines(fixture.text, "Executing function: memcpy"), 1U);
        EXPECT_EQ(fixture.text.find("repeated"), std::string::npos) << fixture.text;
    }

    TEST(ConsoleCoalescing, PeriodicSummaryEveryThousandRepeatsWithCallRanges)
    {
        captured_console fixture({.prepend_call_count = true, .coalesce_repeats = true, .repeat_summary_every = 1000});
        for (uint64_t i = 1; i <= 2001; ++i)
        {
            fixture.console->report(function_call(8, i, "memcpy"));
        }
        EXPECT_EQ(count_lines(fixture.text, "[1] Executing function: memcpy"), 1U);
        EXPECT_EQ(count_lines(fixture.text, "~ tid 8 repeated 1000 more times"), 2U) << fixture.text;
        EXPECT_NE(fixture.text.find("[calls 2..1001]"), std::string::npos) << fixture.text;
        EXPECT_NE(fixture.text.find("[calls 1002..2001]"), std::string::npos) << fixture.text;
    }

    TEST(ConsoleCoalescing, TimeBasedSummaryWhenRepeatsAreSparse)
    {
        captured_console fixture(
            {.coalesce_repeats = true, .repeat_summary_every = 0, .repeat_summary_interval = std::chrono::milliseconds(20)});
        fixture.console->report(function_call(8, 1, "memcpy"));
        fixture.console->report(function_call(8, 2, "memcpy"));
        EXPECT_EQ(fixture.text.find("repeated"), std::string::npos);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        fixture.console->report(function_call(8, 3, "memcpy"));
        EXPECT_NE(fixture.text.find("~ tid 8 repeated 2 more times [calls 2..3]"), std::string::npos) << fixture.text;
    }

    TEST(ConsoleCoalescing, FlushesPendingRepeatsWhenTheKeyChanges)
    {
        captured_console fixture({.coalesce_repeats = true});
        for (uint64_t i = 1; i <= 5; ++i)
        {
            fixture.console->report(function_call(8, i, "memcpy"));
        }
        fixture.console->report(function_call(8, 6, "memset"));
        const auto summary = fixture.text.find("~ tid 8 repeated 4 more times [calls 2..5]: Executing function: memcpy");
        const auto memset_line = fixture.text.find("Executing function: memset");
        ASSERT_NE(summary, std::string::npos) << fixture.text;
        ASSERT_NE(memset_line, std::string::npos) << fixture.text;
        EXPECT_LT(summary, memset_line);
        // Details participate in the identity: a different detail is a new line, not a repeat.
        auto with_detail = function_call(8, 7, "memset");
        with_detail.details.push_back({.label = "size", .value = "0x40"});
        fixture.console->report(with_detail);
        EXPECT_EQ(count_lines(fixture.text, "Executing function: memset"), 2U);
    }

    TEST(ConsoleCoalescing, FlushAtShutdownReportsTheTail)
    {
        captured_console fixture({.coalesce_repeats = true});
        for (uint64_t i = 1; i <= 5; ++i)
        {
            fixture.console->report(function_call(8, i, "memcpy"));
        }
        EXPECT_EQ(fixture.text.find("repeated"), std::string::npos);
        fixture.console->flush();
        EXPECT_NE(fixture.text.find("~ tid 8 repeated 4 more times [calls 2..5]"), std::string::npos) << fixture.text;
        fixture.console->flush();
        EXPECT_EQ(count_lines(fixture.text, "repeated 4 more times"), 1U) << "a second flush must not repeat the summary";
    }

    TEST(ConsoleCoalescing, ThreadsAreCoalescedSeparately)
    {
        captured_console fixture({.coalesce_repeats = true});
        for (uint64_t i = 1; i <= 10; ++i)
        {
            fixture.console->report(function_call(i % 2 ? 8 : 9, i, "memcpy"));
        }
        fixture.console->flush();
        EXPECT_EQ(count_prefixed(fixture.text, "Executing function: memcpy"), 2U) << fixture.text;
        EXPECT_NE(fixture.text.find("~ tid 8 repeated 4 more times"), std::string::npos) << fixture.text;
        EXPECT_NE(fixture.text.find("~ tid 9 repeated 4 more times"), std::string::npos) << fixture.text;
    }

    TEST(ConsoleCoalescing, OtherRoutineEventTypesCoalesceWithoutCallRanges)
    {
        captured_console fixture({.coalesce_repeats = true});
        object_access_event access{};
        access.execution.thread_id = 3;
        access.execution.rip = 0x18002b76c;
        access.execution.rip_module = "ntdll.dll";
        access.type_name = "_KUSER_SHARED_DATA64";
        access.offset = 0x36a;
        access.size = 2;
        access.member_name = "UnparkedProcessorCount";
        for (int i = 0; i < 4; ++i)
        {
            fixture.console->report(access);
        }
        io_control_event control{};
        control.execution.thread_id = 3;
        control.device_name = "SogenGpu";
        control.code = 0x222218;
        fixture.console->report(control);
        fixture.console->report(control);
        fixture.console->flush();
        EXPECT_EQ(count_prefixed(fixture.text, "Object access: _KUSER_SHARED_DATA64"), 1U);
        EXPECT_NE(fixture.text.find("~ tid 3 repeated 3 more times: Object access"), std::string::npos) << fixture.text;
        EXPECT_EQ(count_prefixed(fixture.text, "--> SogenGpu: 0x222218"), 1U);
        EXPECT_NE(fixture.text.find("~ tid 3 repeated 1 more times: --> SogenGpu"), std::string::npos) << fixture.text;
    }

    TEST(ConsoleCoalescing, JsonlReportKeepsEveryEventWhileConsoleCoalesces)
    {
        const auto path = unique_path("sogen-coalesce", ".jsonl");
        const auto cleanup = utils::finally([&] {
            std::error_code error;
            std::filesystem::remove(path, error);
        });
        captured_console fixture({.coalesce_repeats = true});
        auto jsonl = create_jsonl_reporter(path);
        for (uint64_t i = 1; i <= 50; ++i)
        {
            const auto event = function_call(8, i, "memcpy");
            fixture.console->report(event);
            jsonl->report(event);
        }
        jsonl->flush();
        EXPECT_EQ(count_prefixed(fixture.text, "Executing function: memcpy"), 1U);
        EXPECT_EQ(count_prefixed(read_text(path), "{\"type\":\"function_execution\""), 50U);
    }

    // ------------------------------------------------------------------ audit report mode

    TEST(AuditReport, CountsRoutineEventsAndRetainsDiagnostics)
    {
        const auto path = unique_path("sogen-audit", ".jsonl");
        const auto status = unique_path("sogen-audit-status", ".json");
        const auto cleanup = utils::finally([&] {
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::remove(status, error);
        });
        jsonl_report_settings settings{};
        settings.mode = jsonl_report_mode::audit;
        settings.retained_per_key = 3;
        settings.aggregate_interval_events = 1000000;
        settings.aggregate_interval = std::chrono::hours(1);
        settings.status_path = status;
        auto jsonl = create_jsonl_reporter(path, settings);

        for (uint64_t i = 1; i <= 100; ++i)
        {
            jsonl->report(function_call(8, i, "memcpy"));       // interesting: first 3 retained, 97 counted
            jsonl->report(function_call(8, i, "RtlAllocateHeap", false)); // routine: all counted
        }
        object_access_event access{};
        access.execution.thread_id = 8;
        access.execution.rip_module = "ntdll.dll";
        access.type_name = "_PEB64";
        for (int i = 0; i < 10; ++i)
        {
            jsonl->report(access);
        }
        suspicious_activity_event suspicious{};
        suspicious.execution.thread_id = 8;
        suspicious.execution.rip = 0x147e8a8bf;
        suspicious.execution.rip_module = "destiny2.exe";
        suspicious.details = "Illegal instruction";
        jsonl->report(suspicious);
        syscall_event inline_syscall{};
        inline_syscall.execution.thread_id = 8;
        inline_syscall.classification = syscall_classification::inline_syscall;
        inline_syscall.syscall_name = "NtQueryInformationProcess";
        jsonl->report(inline_syscall);
        syscall_event regular{};
        regular.execution.thread_id = 8;
        regular.syscall_name = "NtDeviceIoControlFile";
        jsonl->report(regular);
        jsonl->flush();

        const auto text = read_text(path);
        EXPECT_EQ(count_prefixed(text, "{\"type\":\"function_execution\""), 3U) << text;
        EXPECT_EQ(count_prefixed(text, "{\"type\":\"object_access\""), 0U);
        EXPECT_EQ(count_prefixed(text, "{\"type\":\"suspicious_activity\""), 1U);
        EXPECT_EQ(count_lines(text, "\"class\":\"inline\""), 1U) << text;
        EXPECT_EQ(count_lines(text, "NtDeviceIoControlFile"), 1U) << "regular syscalls only appear inside the aggregate";
        EXPECT_EQ(count_prefixed(text, "{\"type\":\"event_aggregate\""), 1U) << text;
        EXPECT_NE(text.find("\"final\":true"), std::string::npos);
        // uint64 counters are JSON strings, like "ic" and "callCount" elsewhere in the report.
        EXPECT_NE(text.find("\"function_execution\":\"197\""), std::string::npos) << text;
        EXPECT_NE(text.find("\"object_access\":\"10\""), std::string::npos) << text;
        EXPECT_NE(text.find("\"syscall\":\"1\""), std::string::npos) << text;
        EXPECT_NE(text.find("\"key\":\"RtlAllocateHeap (vcruntime140.dll)\",\"count\":\"100\""), std::string::npos) << text;

        const auto status_text = read_text(status);
        EXPECT_NE(status_text.find("\"mode\":\"audit\""), std::string::npos) << status_text;
        EXPECT_NE(status_text.find("\"summarized_events\":\"208\""), std::string::npos) << status_text;
        EXPECT_NE(status_text.find("\"last_location\":{\"rip\":\"0x0\",\"module\":\"<N/A>\",\"tid\":8"), std::string::npos) << status_text;
        EXPECT_NE(status_text.find("\"last_event_type\":\"syscall\""), std::string::npos) << status_text;
    }

    TEST(AuditReport, EmitsAggregateWindowsAndKeepsFullModeUnchanged)
    {
        const auto audit_path = unique_path("sogen-audit-window", ".jsonl");
        const auto full_path = unique_path("sogen-full", ".jsonl");
        const auto cleanup = utils::finally([&] {
            std::error_code error;
            std::filesystem::remove(audit_path, error);
            std::filesystem::remove(full_path, error);
        });
        jsonl_report_settings settings{};
        settings.mode = jsonl_report_mode::audit;
        settings.aggregate_interval_events = 10;
        settings.aggregate_interval = std::chrono::hours(1);
        auto audit = create_jsonl_reporter(audit_path, settings);
        auto full = create_jsonl_reporter(full_path);
        for (uint64_t i = 1; i <= 25; ++i)
        {
            const auto event = function_call(8, i, "RtlAllocateHeap", false);
            audit->report(event);
            full->report(event);
        }
        audit->flush();
        full->flush();
        const auto audit_text = read_text(audit_path);
        EXPECT_EQ(count_prefixed(audit_text, "{\"type\":\"event_aggregate\""), 3U) << audit_text; // 10 + 10 + final 5
        EXPECT_EQ(count_prefixed(audit_text, "{\"type\":\"function_execution\""), 0U);
        EXPECT_NE(audit_text.find("\"windowEvents\":\"5\",\"summarizedEvents\":\"25\""), std::string::npos) << audit_text;
        EXPECT_EQ(count_prefixed(read_text(full_path), "{\"type\":\"function_execution\""), 25U);
    }

    // ------------------------------------------------------------------ logger never throws

#ifdef _WIN32
    TEST(LoggerResilience, FailedConsoleWriterFallsBackToStderrWithoutThrowing)
    {
        const auto path = unique_path("sogen-logger-readonly", ".log");
        {
            std::ofstream create(path, std::ios::binary);
            create << "existing";
        }
        auto* stream = std::fopen(path.string().c_str(), "rb"); // read-only: every fwrite fails
        ASSERT_NE(stream, nullptr);
        const auto cleanup = utils::finally([&] {
            std::fclose(stream);
            std::error_code error;
            std::filesystem::remove(path, error);
        });

        testing::internal::CaptureStderr();
        {
            logger output;
            output.set_console_output(std::make_unique<utils::async_file_writer>(stream));
            for (int i = 0; i < 50; ++i)
            {
                EXPECT_NO_THROW(output.warn("warning %d\n", i));
                EXPECT_NO_THROW(output.error("forced %d\n", i)); // forced lines flush and observe the failure first
            }
            EXPECT_FALSE(output.console_output_failure().empty());
            EXPECT_NO_THROW(output.info("after failure\n"));
        }
        const auto err = testing::internal::GetCapturedStderr();
        EXPECT_NE(err.find("console output writer failed"), std::string::npos) << err;
        EXPECT_NE(err.find("forced 49"), std::string::npos) << err;
        EXPECT_NE(err.find("after failure"), std::string::npos) << err;
        EXPECT_EQ(count_lines(err, "console output writer failed"), 1U) << "the writer is retired once";
    }
#endif
}
