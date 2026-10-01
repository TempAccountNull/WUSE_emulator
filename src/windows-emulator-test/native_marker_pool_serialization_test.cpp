#include "emulation_test_utils.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"
#include "../windows-analyzer/jsonl_reporter.hpp"
#include <utils/finally.hpp>
#include <utils/io.hpp>
#include <atomic>
#include <cstdlib>
#include <fstream>

namespace sogen::test
{
    namespace
    {
        struct previous_marker_budget
        {
            uint64_t started_ns{};
            uint32_t reads{}, bytes{};
            bool time_exhausted{}, clock_invalid{}, read_exhausted{}, byte_exhausted{};
        };

        struct previous_marker_snapshot
        {
            detail::native_marker_reservation reservation{};
            detail::native_marker_identity identity{};
            detail::native_marker_register_snapshot registers{};
            detail::native_marker_resource_sample resources{};
            previous_marker_budget budget{};
            std::array<uint8_t, 8> layout_signature{};
            uint32_t qualification_reads{}, qualification_bytes{};
            uint8_t qualification_available{};
            bool layout_qualified{};
            uint64_t ended_steady_ns{};
            bool complete{};
            std::string error{};
            detail::native_marker_completion completion{};
            detail::native_marker_counts counts{};
        };

        struct previous_debug_event : observation_event
        {
            std::string details{}, transport{};
            std::vector<uint64_t> origin_calls{};
            std::optional<debug_string_cpu_snapshot> cpu_snapshot{};
            std::optional<previous_marker_snapshot> native_marker_capture{};
            uint64_t data_address{}, byte_length{};
            std::string encoding{}, bytes_hex{}, error{};
            std::optional<debug_print_argument> ansi_fallback{};
            uint32_t component{}, level{};
        };

        template <typename Variant>
        struct previous_event_variant;

        template <typename... Event>
        struct previous_event_variant<std::variant<Event...>>
        {
            using type = std::variant<std::conditional_t<std::is_same_v<Event, debug_string_event>, previous_debug_event, Event>...>;
        };

        static_assert(sizeof(detail::native_marker_snapshot) <= sizeof(previous_marker_snapshot) + 64);
        static_assert(sizeof(debug_string_event) <= sizeof(previous_debug_event) + 64);
        static_assert(sizeof(analysis_event) <= sizeof(previous_event_variant<analysis_event>::type) + 64);
        static_assert(std::is_same_v<decltype(detail::native_marker_snapshot{}.request_pool),
                                     std::shared_ptr<const detail::native_marker_pool_sample>>);

        debug_string_event maximal_pool_event()
        {
            debug_string_event event{};
            event.transport = "dbwin";
            event.details = "world_controller:state_manager: Entering state 'cleanup'";
            auto& snapshot = event.native_marker_capture.emplace();
            snapshot.reservation = {detail::native_marker_event::cleanup_entry, 2, 1000};
            snapshot.identity = {77, UINT64_MAX, 9, 0x140000000, 0x3000000};
            snapshot.core_complete = true;
            snapshot.core_ended_steady_ns = 1100;
            snapshot.ended_steady_ns = 1200;
            snapshot.complete = true;
            snapshot.budget.allow_cleanup_pool();
            snapshot.budget.reads = 85;
            snapshot.budget.bytes = 1717;
            snapshot.qualification_reads = 2;
            snapshot.qualification_bytes = 8;
            snapshot.qualification_available = 3;
            snapshot.layout_qualified = true;
            snapshot.layout_signature = {0x41, 0xc6, 0x07, 0x02, 0x83, 0x79, 0x0c, 0xff};
            snapshot.registers.actual_cpu = 1;
            snapshot.registers.actual_tid = 52;
            auto& context = snapshot.registers.context;
            context.available = {UINT64_MAX, (uint64_t{1} << 14) - 1};
            const auto register_bytes = std::as_writable_bytes(std::span{&context, size_t{1}});
            for (const auto& field : detail::marker_register_detail::descriptions)
            {
                for (size_t offset = 0; offset < field.width; ++offset)
                {
                    register_bytes[field.offset + offset] = std::byte{0xff};
                }
            }
            context.stack_requested_words = 16;
            context.stack_status = 0;
            context.stack_base = 0x30000000;
            context.stack_success = {0xffff, 0};
            context.stack.fill(UINT64_MAX);
            const auto fill_resource = []<size_t Size>(detail::native_marker_raw_value<Size>& raw) {
                raw.status = detail::native_marker_read_status::complete;
                raw.bytes.emplace().fill(0xff);
            };
            auto& resources = snapshot.resources;
            fill_resource(resources.resource3);
            fill_resource(resources.resource6);
            fill_resource(resources.outstanding);
            fill_resource(resources.active_pointer);
            fill_resource(resources.registry_pointer);
            fill_resource(resources.registry_vtable);
            fill_resource(resources.registry_object);
            resources.available_mask = 0x7f;
            resources.unavailable_mask = 0;
            resources.resource_reads = 7;
            resources.resource_bytes = 68;
            resources.registry_layout = detail::native_marker_registry_layout::matched;
            resources.registry_mode = UINT32_MAX;
            resources.registry_content_handle = UINT32_MAX;
            detail::native_marker_pool_sample pool{};
            pool.selected = true;
            pool.visited_nodes = 4;
            pool.reads = 62;
            pool.bytes = 1521;
            pool.resolver_matched = true;
            pool.roots_repeat_complete = true;
            pool.roots_repeat_equal = true;
            pool.reason = detail::native_marker_pool_reason::available;
            pool.traversal_reason = detail::native_marker_pool_reason::available;
            const auto fill = []<size_t Size>(detail::native_marker_pool_field<Size>& field) {
                field.address_available = true;
                field.address = UINT64_MAX;
                field.raw.status = detail::native_marker_read_status::complete;
                field.raw.bytes.emplace().fill(0xff);
            };
            fill(pool.resolver);
            fill(pool.directory);
            fill(pool.table);
            fill(pool.directory_repeat);
            fill(pool.table_repeat);
            fill(pool.resource3_repeat);
            for (auto& node : pool.nodes)
            {
                node.handle = UINT32_MAX - 1;
                node.related_handle = UINT32_MAX;
                node.counter = UINT32_MAX;
                node.counter_mask = UINT32_MAX;
                node.config = UINT32_MAX;
                node.reconstructed_handle = UINT32_MAX - 1;
                node.counter_within_mask = true;
                node.generation_bits_match = true;
                node.released = false;
                node.free_tag = false;
                node.linked_mode = true;
                node.bitmap_range_start = UINT16_MAX;
                node.bitmap_range_end = UINT16_MAX;
                node.type = 1;
                node.stored_status = -128;
                node.stored_detail = -1;
                node.repeat_complete = true;
                node.repeat_equal = true;
                node.best_effort_current_owner = true;
                const auto fill_node = [&](auto& raw) {
                    fill(raw.descriptor);
                    fill(raw.allocator);
                    fill(raw.inner);
                    fill(raw.counter);
                    fill(raw.free_link);
                    fill(raw.encoded);
                    fill(raw.header);
                };
                fill_node(node.initial);
                fill_node(node.repeat);
            }
            pool.conditional_stored_status = -128;
            pool.conditional_stored_detail = -1;
            pool.status_node = 3;
            pool.detail_node = 3;
            snapshot.request_pool = std::make_shared<const detail::native_marker_pool_sample>(std::move(pool));
            return event;
        }

        std::string serialize_pool(const debug_string_event& event, const uint32_t fixture_sequence)
        {
            static std::atomic<uint64_t> sequence{};
            const auto path = std::filesystem::temp_directory_path() /
                              ("sogen-pool-serialization-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                               "-" + std::to_string(sequence.fetch_add(1)) + ".jsonl");
            const auto cleanup = utils::finally([&] {
                std::error_code error;
                std::filesystem::remove(path, error);
            });
            auto reporter = create_jsonl_reporter(path, {.mode = jsonl_report_mode::full});
            reporter->report(event);
            reporter->flush();
            const auto raw = utils::io::read_file(path);
            if (const auto* export_directory = std::getenv("SOGEN_NATIVE_MARKER_TEST_OUTPUT_DIR"); export_directory && *export_directory)
            {
                const std::filesystem::path directory{export_directory};
                std::filesystem::create_directories(directory);
                std::ofstream output(directory / ("pool-fixture-" + std::to_string(fixture_sequence) + ".jsonl"),
                                     std::ios::binary | std::ios::trunc);
                output.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
                if (!output)
                {
                    throw std::runtime_error("Failed to export pool serializer fixture");
                }
            }
            return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
        }
    }

    TEST(NativeMarkerPoolSerialization, SnapshotAndEventGrowthStayWithinPointerAllowance)
    {
        RecordProperty("snapshot_before", std::to_string(sizeof(previous_marker_snapshot)));
        RecordProperty("snapshot_after", std::to_string(sizeof(detail::native_marker_snapshot)));
        RecordProperty("debug_event_before", std::to_string(sizeof(previous_debug_event)));
        RecordProperty("debug_event_after", std::to_string(sizeof(debug_string_event)));
        RecordProperty("analysis_event_before", std::to_string(sizeof(previous_event_variant<analysis_event>::type)));
        RecordProperty("analysis_event_after", std::to_string(sizeof(analysis_event)));
        RecordProperty("pool_payload", std::to_string(sizeof(detail::native_marker_pool_sample)));
        EXPECT_FALSE(detail::native_marker_snapshot{}.request_pool);
        EXPECT_LE(sizeof(detail::native_marker_snapshot) - sizeof(previous_marker_snapshot), size_t{64});
    }

    TEST(NativeMarkerPoolSerialization, EventCopiesShareImmutablePayloadAndKeepCoreWitness)
    {
        auto first = maximal_pool_event();
        auto second = first;
        ASSERT_TRUE(first.native_marker_capture->request_pool);
        EXPECT_EQ(first.native_marker_capture->request_pool.get(), second.native_marker_capture->request_pool.get());
        first.native_marker_capture->request_pool.reset();
        EXPECT_TRUE(second.native_marker_capture->request_pool);
        second.native_marker_capture->complete = false;
        second.native_marker_capture->budget.time_exhausted = true;
        auto late_pool = *second.native_marker_capture->request_pool;
        detail::native_marker_pool_unqualify(late_pool, detail::native_marker_pool_reason::time_or_clock_limit);
        second.native_marker_capture->request_pool = std::make_shared<const detail::native_marker_pool_sample>(std::move(late_pool));
        EXPECT_TRUE(second.native_marker_capture->core_complete);
        const auto json = serialize_pool(second, 0);
        EXPECT_NE(json.find("\"complete\":false"), std::string::npos);
        EXPECT_NE(json.find("\"core_complete\":true"), std::string::npos);
    }

    TEST(NativeMarkerPoolSerialization, MaximumPayloadHasExactFieldCountAndFitsExistingLineCap)
    {
        const auto json = serialize_pool(maximal_pool_event(), 1);
        size_t count{}, offset{};
        while ((offset = json.find("\"raw_bytes_hex\"", offset)) != std::string::npos)
        {
            ++count;
            ++offset;
        }
        EXPECT_EQ(count, size_t{165});
        EXPECT_LT(json.size(), size_t{65536});
        EXPECT_LT(json.size(), size_t{1048576});
        EXPECT_NE(json.find("\"max_total_memory_reads\":87"), std::string::npos);
        EXPECT_NE(json.find("\"max_total_memory_bytes\":1725"), std::string::npos);
        EXPECT_NE(json.find("\"stored_status\":-128"), std::string::npos);
        EXPECT_NE(json.find("\"native_getter_executed\":false"), std::string::npos);
        RecordProperty("maximum_fixture_line_bytes", std::to_string(json.size()));
    }

    TEST(NativeMarkerPoolSerialization, NullPayloadSerializesNotSelectedWithoutNodeEnvelopes)
    {
        auto event = maximal_pool_event();
        event.native_marker_capture->request_pool.reset();
        const auto json = serialize_pool(event, 2);
        EXPECT_NE(json.find("\"request_pool\":{\"selected\":false"), std::string::npos);
        EXPECT_EQ(json.find("\"nodes\":"), std::string::npos);
        EXPECT_EQ(json.find("\"full_handle\":"), std::string::npos);
    }
}
