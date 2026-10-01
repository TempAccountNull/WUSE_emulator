#include "emulation_test_utils.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"
#include "../windows-analyzer/jsonl_reporter.hpp"
#include <utils/finally.hpp>
#include <utils/io.hpp>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>

namespace sogen::test
{
    namespace
    {
        detail::native_marker_snapshot marker_snapshot()
        {
            detail::native_marker_snapshot snapshot{};
            snapshot.reservation = {detail::native_marker_event::cleanup_entry, 2, (uint64_t{1} << 63) + 3};
            snapshot.identity = {.pid = 77, .birth = UINT64_MAX, .generation = 9, .module_base = 0x140000000, .module_size = 0x2000000};
            snapshot.ended_steady_ns = snapshot.reservation.sample_steady_ns + 50;
            snapshot.complete = true;
            snapshot.layout_qualified = true;
            snapshot.qualification_reads = 2;
            snapshot.qualification_bytes = 8;
            snapshot.qualification_available = 3;
            snapshot.counts = {200, 2, 2, 2, 0, 1, 1, 100};
            snapshot.layout_signature = {0x41, 0xc6, 0x07, 0x02, 0x83, 0x79, 0x0c, 0xff};
            snapshot.completion = {true, true, detail::native_marker_stop::completed};
            snapshot.registers.actual_cpu = 3;
            snapshot.registers.actual_tid = 52;
            auto& context = snapshot.registers.context;
            context.available = {UINT64_MAX, (uint64_t{1} << 14) - 1};
            auto bytes = std::as_writable_bytes(std::span{&context, size_t{1}});
            for (size_t index = 0; index < detail::marker_register_detail::descriptions.size(); ++index)
            {
                const auto& field = detail::marker_register_detail::descriptions[index];
                for (size_t offset = 0; offset < field.width; ++offset)
                {
                    bytes[field.offset + offset] = static_cast<std::byte>((index * 17 + offset) & 255);
                }
            }
            context.gpr[7] = 0x30000000;
            context.rip = 0x140123456;
            context.rflags = 0x102;
            context.stack_requested_words = 16;
            context.stack_status = 0;
            context.stack_base = context.gpr[7];
            context.stack_success = {0xffff, 0};
            for (size_t index = 0; index < 16; ++index)
            {
                context.stack[index] = index ? 0x1111000000000000 + index : 0;
            }
            const auto full = []<size_t Size>(detail::native_marker_raw_value<Size>& field) {
                field.status = detail::native_marker_read_status::complete;
                field.bytes.emplace();
                for (size_t index = 0; index < Size; ++index)
                {
                    (*field.bytes)[index] = static_cast<uint8_t>(index);
                }
            };
            auto& resources = snapshot.resources;
            full(resources.resource3);
            resources.resource3.bytes = std::array<uint8_t, 12>{0x12, 0x34, 0xaa, 0xbb, 0xff, 0xff, 0xff, 0xff, 0x78, 0x56, 0x34, 0x12};
            full(resources.resource6);
            full(resources.outstanding);
            resources.outstanding.bytes = std::array<uint8_t, 4>{0xff, 0xff, 0xff, 0xff};
            full(resources.active_pointer);
            resources.active_pointer.bytes = std::array<uint8_t, 8>{};
            full(resources.registry_pointer);
            resources.registry_pointer.bytes = std::array<uint8_t, 8>{0, 0, 0, 0x40, 0, 0, 0, 0};
            full(resources.registry_vtable);
            resources.registry_vtable.bytes = std::array<uint8_t, 8>{0x68, 0xb5, 0xbe, 0x41, 1, 0, 0, 0};
            full(resources.registry_object);
            resources.registry_object.bytes =
                std::array<uint8_t, 16>{0x68, 0xb5, 0xbe, 0x41, 1, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0xff, 0xff, 0xff};
            resources.registry_layout = detail::native_marker_registry_layout::matched;
            resources.registry_mode = 0x80000000;
            resources.registry_content_handle = 0xffffffff;
            resources.available_mask = 0x7f;
            resources.unavailable_mask = 0;
            resources.resource_reads = 7;
            resources.resource_bytes = 68;
            snapshot.budget.started_ns = snapshot.reservation.sample_steady_ns;
            snapshot.budget.reads = 23;
            snapshot.budget.bytes = 196;
            return snapshot;
        }

        detail::native_marker_snapshot partial_snapshot()
        {
            auto snapshot = marker_snapshot();
            snapshot.complete = false;
            snapshot.layout_qualified = false;
            snapshot.qualification_available = 1;
            snapshot.counts.complete_captures = 1;
            snapshot.counts.partial_captures = 1;
            snapshot.error = "read failed: \"quoted\"\nnext line";
            snapshot.completion.complete = false;
            auto& context = snapshot.registers.context;
            context.available = {(uint64_t{1} << 0) | (uint64_t{1} << 7), 0};
            context.read_errors = {uint64_t{1} << 1, uint64_t{1} << 13};
            context.gpr[0] = 0;
            context.gpr[1] = UINT64_MAX;
            context.stack_status = 1;
            context.stack_success = {(uint64_t{1} << 0) | (uint64_t{1} << 2) | (uint64_t{1} << 15), 0};
            snapshot.resources.resource6.bytes.reset();
            snapshot.resources.resource6.status = detail::native_marker_read_status::read_failed;
            snapshot.resources.registry_vtable.bytes.reset();
            snapshot.resources.registry_vtable.status = detail::native_marker_read_status::passive_span_rejected;
            snapshot.resources.registry_object.bytes.reset();
            snapshot.resources.registry_object.status = detail::native_marker_read_status::dependency_unavailable;
            snapshot.resources.registry_layout = detail::native_marker_registry_layout::unavailable;
            snapshot.resources.registry_mode.reset();
            snapshot.resources.registry_content_handle.reset();
            snapshot.resources.available_mask = 0x1d;
            snapshot.resources.unavailable_mask = 0x62;
            snapshot.budget.time_exhausted = true;
            return snapshot;
        }

        debug_string_event print_event()
        {
            debug_string_event event{};
            event.execution.thread_id = 52;
            event.execution.rip = 0x18009d812;
            event.execution.rip_module = "ntdll.dll";
            event.details = "unchanged native print\r\n";
            event.transport = "dbwin";
            event.encoding = "windows-1252";
            return event;
        }

        std::string serialize_fixture(const std::string_view name, const std::vector<debug_string_event>& events,
                                      const jsonl_report_mode mode = jsonl_report_mode::full)
        {
            static std::atomic<uint64_t> next_id{};
            const auto path = std::filesystem::temp_directory_path() /
                              ("sogen-native-marker-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                               std::to_string(next_id.fetch_add(1)) + ".jsonl");
            const auto cleanup = utils::finally([&] {
                std::error_code error;
                std::filesystem::remove(path, error);
            });
            jsonl_report_settings settings{};
            settings.mode = mode;
            settings.dedupe = true;
            auto reporter = create_jsonl_reporter(path, settings);
            for (const auto& event : events)
            {
                reporter->report(event);
            }
            reporter->flush();
            const auto raw = utils::io::read_file(path);
            std::string result(reinterpret_cast<const char*>(raw.data()), raw.size());
            if (const auto* export_directory = std::getenv("SOGEN_NATIVE_MARKER_TEST_OUTPUT_DIR"); export_directory && *export_directory)
            {
                const std::filesystem::path directory{export_directory};
                std::filesystem::create_directories(directory);
                std::ofstream output(directory / name, std::ios::binary | std::ios::trunc);
                output.write(result.data(), static_cast<std::streamsize>(result.size()));
                if (!output)
                {
                    throw std::runtime_error("Failed to export native marker reporter fixture");
                }
            }
            return result;
        }
    }

    TEST(NativeMarkerSerialization, ExportsCompleteReporterFixtures)
    {
        auto complete = print_event();
        complete.native_marker_capture = marker_snapshot();
        auto partial = print_event();
        partial.native_marker_capture = partial_snapshot();
        const auto plain = print_event();
        for (const auto& [name, event] : std::array<std::pair<std::string_view, debug_string_event>, 3>{
                 {{"all-fields.jsonl", complete}, {"partial-fields.jsonl", partial}, {"no-marker.jsonl", plain}}})
        {
            const auto json = serialize_fixture(name, {event});
            EXPECT_FALSE(json.empty());
            EXPECT_TRUE(json.ends_with('\n'));
            EXPECT_LT(json.size(), 65536U);
        }
        for (const auto& [name, mode] : std::array<std::pair<std::string_view, jsonl_report_mode>, 2>{
                 {{"full-dedupe.jsonl", jsonl_report_mode::full}, {"audit-dedupe.jsonl", jsonl_report_mode::audit}}})
        {
            const auto json = serialize_fixture(name, {complete, partial, plain, plain}, mode);
            EXPECT_EQ(std::ranges::count(json, '\n'), 4);
        }
        auto long_error = complete;
        long_error.native_marker_capture->error.assign(2048, 'x');
        serialize_fixture("long-error.jsonl", {long_error});
        auto unicode_error = complete;
        unicode_error.native_marker_capture->error = std::string(511, 'x') + "\xe2\x82\xac" + std::string(16, 'y');
        serialize_fixture("unicode-error.jsonl", {unicode_error});
        auto no_qualification = complete;
        no_qualification.native_marker_capture->qualification_available = 0;
        no_qualification.native_marker_capture->layout_qualified = false;
        serialize_fixture("no-qualification.jsonl", {no_qualification});
        auto core_only = complete;
        auto& core_resources = core_only.native_marker_capture->resources;
        core_resources.registry_pointer.bytes = std::array<uint8_t, 8>{};
        core_resources.registry_vtable.bytes.reset();
        core_resources.registry_vtable.status = detail::native_marker_read_status::dependency_unavailable;
        core_resources.registry_object.bytes.reset();
        core_resources.registry_object.status = detail::native_marker_read_status::dependency_unavailable;
        core_resources.registry_layout = detail::native_marker_registry_layout::unavailable;
        core_resources.registry_mode.reset();
        core_resources.registry_content_handle.reset();
        core_resources.available_mask = 0x1f;
        core_resources.unavailable_mask = 0x60;
        core_resources.resource_reads = 5;
        core_resources.resource_bytes = 44;
        core_only.native_marker_capture->budget.reads = 21;
        core_only.native_marker_capture->budget.bytes = 172;
        serialize_fixture("core-only.jsonl", {core_only});
        auto zero_optionals = complete;
        zero_optionals.native_marker_capture->resources.registry_mode = 0;
        zero_optionals.native_marker_capture->resources.registry_content_handle = 0;
        auto& zero_header = *zero_optionals.native_marker_capture->resources.registry_object.bytes;
        std::fill(zero_header.begin() + 8, zero_header.end(), 0);
        serialize_fixture("zero-optionals.jsonl", {zero_optionals});
        auto mismatch = complete;
        mismatch.native_marker_capture->resources.registry_layout = detail::native_marker_registry_layout::vtable_mismatch;
        (*mismatch.native_marker_capture->resources.registry_object.bytes)[0] = 0;
        serialize_fixture("layout-mismatch.jsonl", {mismatch});
    }

    TEST(NativeMarkerSerialization, ConsoleContentHashExcludesOnlyMarkerPayload)
    {
        const auto plain = print_event();
        auto with_marker = plain;
        with_marker.native_marker_capture = marker_snapshot();
        EXPECT_EQ(event_content_hash(plain), event_content_hash(with_marker));
        with_marker.native_marker_capture = partial_snapshot();
        EXPECT_EQ(event_content_hash(plain), event_content_hash(with_marker));
        with_marker.details += "changed";
        EXPECT_NE(event_content_hash(plain), event_content_hash(with_marker));
        with_marker = plain;
        with_marker.native_marker_capture = marker_snapshot();
        with_marker.execution.thread_id = 53;
        EXPECT_NE(event_content_hash(plain), event_content_hash(with_marker));
        with_marker = plain;
        with_marker.native_marker_capture = marker_snapshot();
        with_marker.error = "legacy capture failure";
        EXPECT_NE(event_content_hash(plain), event_content_hash(with_marker));
        with_marker = plain;
        with_marker.native_marker_capture = marker_snapshot();
        with_marker.cpu_snapshot.emplace().instruction_pointer = 0x140000111;
        EXPECT_NE(event_content_hash(plain), event_content_hash(with_marker));
    }

    TEST(NativeMarkerSerialization, EventOwnsSnapshotAndPreservesLegacyOodleState)
    {
        auto event = print_event();
        auto snapshot = marker_snapshot();
        event.native_marker_capture = snapshot;
        event.cpu_snapshot.emplace().stack_words = {0x1234, 0};
        snapshot.registers.context.gpr[0] = 0;
        snapshot.resources.resource3.bytes.reset();
        EXPECT_NE(event.native_marker_capture->registers.context.gpr[0], snapshot.registers.context.gpr[0]);
        EXPECT_TRUE(event.native_marker_capture->resources.resource3.bytes.has_value());
        ASSERT_TRUE(event.cpu_snapshot.has_value());
        EXPECT_EQ(event.cpu_snapshot->stack_words, (std::vector<uint64_t>{0x1234, 0}));
    }
}
