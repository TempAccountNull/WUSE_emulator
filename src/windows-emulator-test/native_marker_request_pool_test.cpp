#include "emulation_test_utils.hpp"
#include <native_marker_request_pool.hpp>
#include <native_marker_capture_state.hpp>
#include <native_child_sample.hpp>
#include "../windows-analyzer/analysis_reporter.hpp"
#include "../windows-analyzer/analysis.hpp"
#include "../windows-analyzer/snapshot.hpp"
#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <native_child_capture.hpp>
#include <windows_emulator.hpp>
#include <platform/win_pefile.hpp>
#include <utils/finally.hpp>
#include <utils/function.hpp>
#include <algorithm>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <tuple>
#include <vector>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>

using namespace sogen::detail;

namespace
{
    struct pool_kernel
    {
        bool held{true};
        bool throws{};

        bool is_held_by_current_thread() const
        {
            if (throws)
            {
                throw std::runtime_error("kernel");
            }
            return held;
        }
    };

    struct pool_read
    {
        uint64_t address{};
        size_t size{};
        bool operator==(const pool_read&) const = default;
    };

    struct pool_fixture
    {
        static constexpr uint64_t base = 0x140000000;
        static constexpr uint64_t image_size = 0x3000000;
        static constexpr uint64_t directory = 0x20000000;
        static constexpr uint64_t table = 0x21000000;
        static constexpr uint32_t parent = 0x0cf8c000;
        static constexpr uint32_t child = 0x06f8e001;
        pool_kernel kernel{};
        native_marker_resource_sample resources{};
        native_marker_read_budget budget{1000};
        uint64_t now{1000};
        bool metadata_throws{};
        bool clock_throws{};
        bool reader_throws{};
        uint64_t advance_after_read{};
        uint64_t advance_after_metadata{};
        pool_read failed{};
        pool_read rejected{};
        std::map<uint64_t, uint8_t> memory{};
        std::vector<pool_read> reads{};
        std::vector<pool_read> guards{};
        std::map<std::pair<uint64_t, size_t>, uint32_t> read_counts{};
        std::function<void(pool_read, uint32_t)> before_read{};
        std::function<void()> before_clock{};

        pool_fixture()
        {
            budget.reads = 23;
            budget.bytes = 196;
            budget.allow_cleanup_pool();
            set<uint64_t>(base + 0x2439c70, directory);
            set<uint64_t>(directory, table);
            for (size_t index = 0; index < native_marker_pool_resolver_bytes.size(); ++index)
            {
                memory[base + 0x42c669 + index] = native_marker_pool_resolver_bytes[index];
            }
            std::array<uint8_t, 12> root{};
            root[0] = 1;
            for (size_t index = 0; index < 4; ++index)
            {
                root[4 + index] = static_cast<uint8_t>(parent >> (index * 8));
            }
            resources.resource3.bytes = root;
            resources.resource3.status = native_marker_read_status::complete;
            for (size_t index = 0; index < root.size(); ++index)
            {
                memory[base + 0x1fb5f44 + index] = root[index];
            }
            add_node(0, parent, child, 2, 1, -3);
            add_node(1, child, 0xffffffffU, 1, -1, -128);
        }

        template <typename Integer>
        void set(const uint64_t address, const Integer value)
        {
            static_assert(std::is_unsigned_v<Integer>);
            for (size_t index = 0; index < sizeof(Integer); ++index)
            {
                memory[address + index] = static_cast<uint8_t>(value >> (index * 8));
            }
        }

        void zero(const uint64_t address, const size_t size)
        {
            for (size_t index = 0; index < size; ++index)
            {
                memory[address + index] = 0;
            }
        }

        static uint64_t allocation(const uint32_t ordinal)
        {
            return 0x22000000ULL + ordinal * 0x1000ULL;
        }

        static uint64_t inner(const uint32_t ordinal)
        {
            return 0x23000000ULL + ordinal * 0x1000ULL;
        }

        static uint64_t storage(const uint32_t ordinal)
        {
            return 0x24000000ULL + ordinal * 0x1000ULL;
        }

        static uint64_t data(const uint32_t ordinal)
        {
            return 0x25000000ULL + ordinal * 0x1000ULL;
        }

        static uint64_t free_data(const uint32_t ordinal)
        {
            return 0x26000000ULL + ordinal * 0x1000ULL;
        }

        static uint64_t descriptor(const uint32_t handle)
        {
            return table + uint64_t{native_marker_pool_group(handle)} * 0x40;
        }

        static uint64_t header(const uint32_t ordinal, const uint32_t handle)
        {
            return data(ordinal) + (handle & 0x1fffU) * 0xc8ULL;
        }

        static uint64_t counter(const uint32_t ordinal, const uint32_t handle)
        {
            return storage(ordinal) + 0x80 + (handle & 0x1fffU) * 4ULL;
        }

        static uint64_t free_link(const uint32_t ordinal, const uint32_t handle)
        {
            return free_data(ordinal) + 0x40 + (handle & 0x1fffU) * 0xc8ULL;
        }

        void add_node(const uint32_t ordinal, const uint32_t handle, const uint32_t related, const uint8_t type, const int8_t status,
                      const int8_t detail)
        {
            const auto desc = descriptor(handle);
            zero(desc, 56);
            set<uint64_t>(desc + 8, data(ordinal));
            set<uint64_t>(desc + 0x10, allocation(ordinal));
            set<uint32_t>(desc + 0x30, 0xc8);
            zero(allocation(ordinal), 56);
            set<uint64_t>(allocation(ordinal), inner(ordinal));
            set<uint64_t>(allocation(ordinal) + 8, storage(ordinal));
            set<uint32_t>(allocation(ordinal) + 0x10, 0xc8);
            set<uint32_t>(allocation(ordinal) + 0x1c, 0x80);
            set<uint32_t>(allocation(ordinal) + 0x20, 4);
            set<uint32_t>(allocation(ordinal) + 0x24, 0xff);
            set<uint32_t>(allocation(ordinal) + 0x34, (handle >> 13) & 0x3ff);
            set<uint32_t>(counter(ordinal, handle), (handle >> 23) & 0xff);
            zero(inner(ordinal), 32);
            set<uint64_t>(inner(ordinal), free_data(ordinal));
            set<uint32_t>(inner(ordinal) + 0x10, 0xc8);
            set<uint32_t>(inner(ordinal) + 0x14, 0x40);
            set<uint32_t>(free_link(ordinal, handle), 0);
            const auto view = header(ordinal, handle);
            zero(view, 16);
            memory[view + 6] = type;
            memory[view + 7] = std::bit_cast<uint8_t>(status);
            memory[view + 0xa] = std::bit_cast<uint8_t>(detail);
            set<uint32_t>(view + 0xc, related);
        }

        native_marker_pool_sample sample(const uint64_t module_base = base, const uint64_t module_size = image_size)
        {
            auto passive = [&](const uint64_t address, const size_t size) {
                guards.push_back({address, size});
                now += advance_after_metadata;
                if (metadata_throws)
                {
                    throw std::runtime_error("metadata");
                }
                return rejected != pool_read{address, size};
            };
            auto reader = [&](const uint64_t address, void* destination, const size_t size) {
                reads.push_back({address, size});
                const auto count = ++read_counts[{address, size}];
                if (before_read)
                {
                    before_read({address, size}, count);
                }
                auto* output = static_cast<uint8_t*>(destination);
                std::fill(output, output + size, uint8_t{0xaa});
                now += advance_after_read;
                if (reader_throws)
                {
                    throw std::runtime_error("reader");
                }
                if (failed == pool_read{address, size})
                {
                    return false;
                }
                for (size_t index = 0; index < size; ++index)
                {
                    const auto found = memory.find(address + index);
                    if (found == memory.end())
                    {
                        return false;
                    }
                    output[index] = found->second;
                }
                return true;
            };
            auto clock = [&] {
                if (before_clock)
                {
                    before_clock();
                }
                if (clock_throws)
                {
                    throw std::runtime_error("clock");
                }
                return now;
            };
            return sample_native_marker_request_pool(module_base, module_size, resources, kernel, budget, passive, reader, clock);
        }

        native_child_selection select_child()
        {
            add_node(1, child, parent, 3, 1, 0);
            native_marker_snapshot marker{};
            marker.identity = {1, 2, 3, base, image_size};
            marker.reservation = {native_marker_event::cleanup_entry, 2, now};
            marker.complete = true;
            marker.core_complete = true;
            marker.resources = resources;
            marker.request_pool = std::make_shared<const native_marker_pool_sample>(sample());
            const auto selected = select_native_child(marker);
            if (!selected)
            {
                throw std::runtime_error("child selection");
            }
            reads.clear();
            guards.clear();
            read_counts.clear();
            return *selected;
        }

        native_marker_register_snapshot child_registers(const native_child_selection& selected, const uint32_t site,
                                                        const int32_t effective = 1)
        {
            constexpr uint64_t context = 0x28000000;
            constexpr uint64_t manager = 0x29000000;
            native_marker_register_snapshot registers{};
            registers.actual_tid = 7;
            registers.context.available[0] = 0x3ffff;
            registers.context.rip = base + native_child_limits::rvas[site];
            auto& gpr = registers.context.gpr;
            if (!site)
            {
                gpr[0] = 0x19;
                gpr[1] = context;
                gpr[3] = 0xDEADBEEF;
                gpr[5] = selected.child_view;
                gpr[15] = manager;
                set<uint32_t>(manager + 0x220068, selected.child_handle);
                set<uint16_t>(context, 0x1234);
            }
            else
            {
                gpr[0] = 0x7f;
                gpr[1] = std::bit_cast<uint32_t>(effective);
                gpr[2] = manager;
                gpr[3] = selected.parent_handle;
                gpr[5] = selected.parent_view;
                gpr[6] = 3;
                gpr[14] = base + 0x1FB5F20;
                gpr[15] = base + 0x1FB5F44;
            }
            return registers;
        }

        native_child_sample sample_child(const native_child_selection& selected, const uint32_t site,
                                         const native_marker_register_snapshot& registers)
        {
            auto passive = [&](const uint64_t address, const size_t size) {
                guards.push_back({address, size});
                now += advance_after_metadata;
                if (metadata_throws)
                {
                    throw std::runtime_error("metadata");
                }
                return rejected != pool_read{address, size};
            };
            auto reader = [&](const uint64_t address, void* destination, const size_t size) {
                reads.push_back({address, size});
                const auto count = ++read_counts[{address, size}];
                if (before_read)
                {
                    before_read({address, size}, count);
                }
                now += advance_after_read;
                if (reader_throws)
                {
                    throw std::runtime_error("reader");
                }
                if (failed == pool_read{address, size})
                {
                    return false;
                }
                auto* output = static_cast<uint8_t*>(destination);
                for (size_t index = 0; index < size; ++index)
                {
                    const auto found = memory.find(address + index);
                    if (found == memory.end())
                    {
                        return false;
                    }
                    output[index] = found->second;
                }
                return true;
            };
            auto clock = [&] {
                if (before_clock)
                {
                    before_clock();
                }
                if (clock_throws)
                {
                    throw std::runtime_error("clock");
                }
                return now;
            };
            return sample_native_child(selected, site, registers, kernel, passive, reader, clock, now);
        }
    };
}

TEST(NativeMarkerRequestPool, TwoNodesUseExactBoundsAndSignedValues)
{
    pool_fixture fixture;
    const auto result = fixture.sample();
    ASSERT_EQ(result.visited_nodes, uint32_t{2});
    EXPECT_EQ(result.reads, uint32_t{34});
    EXPECT_EQ(result.bytes, uint32_t{817});
    EXPECT_EQ(fixture.budget.reads, uint32_t{57});
    EXPECT_EQ(fixture.budget.bytes, uint32_t{1013});
    EXPECT_EQ(result.nodes[0].handle, std::optional<uint32_t>{pool_fixture::parent});
    EXPECT_EQ(result.nodes[1].handle, std::optional<uint32_t>{pool_fixture::child});
    EXPECT_EQ(result.conditional_stored_status, std::optional<int8_t>{-1});
    EXPECT_EQ(result.conditional_stored_detail, std::optional<int8_t>{-128});
    EXPECT_EQ(result.status_node, std::optional<uint32_t>{1});
    EXPECT_TRUE(result.roots_repeat_equal);
    EXPECT_TRUE(result.nodes[0].best_effort_current_owner);
    EXPECT_FALSE(result.atomic);
    EXPECT_FALSE(result.strong_lifetime);
    EXPECT_EQ(fixture.guards, fixture.reads);
}

TEST(NativeMarkerRequestPool, FourNodesUseExactMaximumAndReverseContinuity)
{
    pool_fixture fixture;
    const uint32_t third = native_marker_pool_handle(7, 0x3c8, 2);
    const uint32_t fourth = native_marker_pool_handle(8, 0x3c9, 3);
    fixture.add_node(1, pool_fixture::child, third, 2, 1, 4);
    fixture.add_node(2, third, fourth, 2, 1, 5);
    fixture.add_node(3, fourth, 0xffffffffU, 1, 2, -2);
    const auto result = fixture.sample();
    EXPECT_EQ(result.visited_nodes, uint32_t{4});
    EXPECT_EQ(result.reads, native_marker_pool_sample::max_reads);
    EXPECT_EQ(result.bytes, native_marker_pool_sample::max_bytes);
    EXPECT_EQ(fixture.budget.reads, uint32_t{85});
    EXPECT_EQ(fixture.budget.bytes, uint32_t{1717});
    EXPECT_EQ(result.conditional_stored_status, std::optional<int8_t>{2});
    ASSERT_EQ(fixture.reads.size(), size_t{62});
    for (size_t index = 0; index < 4; ++index)
    {
        EXPECT_EQ(fixture.reads[31 + index * 7].address, result.nodes[3 - index].initial.header.address);
        EXPECT_EQ(fixture.reads[32 + index * 7].address, result.nodes[3 - index].initial.encoded.address);
        EXPECT_EQ(fixture.reads[33 + index * 7].address, result.nodes[3 - index].initial.free_link.address);
        EXPECT_EQ(fixture.reads[34 + index * 7].address, result.nodes[3 - index].initial.counter.address);
        EXPECT_EQ(fixture.reads[35 + index * 7].address, result.nodes[3 - index].initial.inner.address);
        EXPECT_EQ(fixture.reads[36 + index * 7].address, result.nodes[3 - index].initial.allocator.address);
        EXPECT_EQ(fixture.reads[37 + index * 7].address, result.nodes[3 - index].initial.descriptor.address);
    }
    EXPECT_EQ(fixture.reads[59], (pool_read{pool_fixture::directory, 8}));
    EXPECT_EQ(fixture.reads[60], (pool_read{pool_fixture::base + 0x2439c70, 8}));
    EXPECT_EQ(fixture.reads[61], (pool_read{pool_fixture::base + 0x1fb5f44, 12}));
}

TEST(NativeMarkerRequestPool, DepthBoundPreservesNextFullHandle)
{
    pool_fixture fixture;
    const uint32_t third = native_marker_pool_handle(7, 0x3c8, 2);
    const uint32_t fourth = native_marker_pool_handle(8, 0x3c9, 3);
    const uint32_t fifth = native_marker_pool_handle(9, 0x3ca, 4);
    fixture.add_node(1, pool_fixture::child, third, 2, 1, 4);
    fixture.add_node(2, third, fourth, 2, 1, 5);
    fixture.add_node(3, fourth, fifth, 2, 1, 6);
    const auto result = fixture.sample();
    EXPECT_EQ(result.traversal_reason, native_marker_pool_reason::traversal_required);
    EXPECT_EQ(result.nodes[3].related_handle, std::optional<uint32_t>{fifth});
    EXPECT_FALSE(result.conditional_stored_status);
    EXPECT_FALSE(result.conditional_stored_detail);
    EXPECT_EQ(result.reads, uint32_t{62});
}

TEST(NativeMarkerRequestPool, CyclesDoNotDereferenceAnotherNode)
{
    pool_fixture fixture;
    fixture.set<uint32_t>(pool_fixture::header(1, pool_fixture::child) + 0xc, pool_fixture::parent);
    fixture.memory[pool_fixture::header(1, pool_fixture::child) + 6] = 2;
    fixture.memory[pool_fixture::header(1, pool_fixture::child) + 7] = 1;
    const auto result = fixture.sample();
    EXPECT_EQ(result.visited_nodes, uint32_t{2});
    EXPECT_EQ(result.traversal_reason, native_marker_pool_reason::cycle);
    EXPECT_FALSE(result.conditional_stored_status);
    EXPECT_FALSE(result.conditional_stored_detail);
}

TEST(NativeMarkerRequestPool, StatusAndDetailTraverseIndependently)
{
    pool_fixture fixture;
    fixture.memory[pool_fixture::header(0, pool_fixture::parent) + 7] = 0xff;
    const auto result = fixture.sample();
    EXPECT_EQ(result.conditional_stored_status, std::optional<int8_t>{-1});
    EXPECT_EQ(result.status_node, std::optional<uint32_t>{0});
    EXPECT_EQ(result.conditional_stored_detail, std::optional<int8_t>{-128});
    EXPECT_EQ(result.detail_node, std::optional<uint32_t>{1});
}

TEST(NativeMarkerRequestPool, TerminalStatusSurvivesUnavailableDeeperDetail)
{
    pool_fixture fixture;
    fixture.memory[pool_fixture::header(0, pool_fixture::parent) + 7] = 0xfe;
    fixture.failed = {pool_fixture::counter(1, pool_fixture::child), 4};
    const auto result = fixture.sample();
    EXPECT_EQ(result.conditional_stored_status, std::optional<int8_t>{-2});
    EXPECT_FALSE(result.conditional_stored_detail);
    EXPECT_EQ(result.detail_reason, native_marker_pool_reason::node_unavailable);
}

TEST(NativeMarkerRequestPool, ResolverMismatchStopsBeforeHeapReads)
{
    pool_fixture fixture;
    fixture.memory[pool_fixture::base + 0x42c669] ^= 1;
    const auto result = fixture.sample();
    EXPECT_EQ(result.reason, native_marker_pool_reason::resolver_mismatch);
    EXPECT_EQ(result.reads, uint32_t{1});
    EXPECT_EQ(result.bytes, uint32_t{69});
    EXPECT_FALSE(result.directory.raw.bytes);
}

TEST(NativeMarkerRequestPool, ReleasedAndFreeSlotsRejectOwnerWithoutFollowingChild)
{
    pool_fixture released;
    released.memory[pool_fixture::header(0, pool_fixture::parent) + 8] = 1;
    const auto first = released.sample();
    EXPECT_EQ(first.nodes[0].reason, native_marker_pool_reason::released);
    EXPECT_EQ(first.visited_nodes, uint32_t{1});
    EXPECT_FALSE(first.conditional_stored_status);
    pool_fixture free;
    free.set<uint32_t>(pool_fixture::free_link(0, pool_fixture::parent), 0xfefe0001);
    const auto second = free.sample();
    EXPECT_EQ(second.nodes[0].reason, native_marker_pool_reason::free_slot);
    EXPECT_FALSE(second.nodes[0].best_effort_current_owner);
    EXPECT_EQ(second.nodes[0].initial.header.raw.bytes->at(2), uint8_t{0});
}

TEST(NativeMarkerRequestPool, HeaderFefeBytesAreNotFreeMetadata)
{
    pool_fixture fixture;
    fixture.set<uint32_t>(pool_fixture::header(0, pool_fixture::parent), 0xfefe0001);
    const auto result = fixture.sample();
    EXPECT_EQ(result.nodes[0].free_tag, std::optional<bool>{false});
    EXPECT_TRUE(result.nodes[0].best_effort_current_owner);
}

TEST(NativeMarkerRequestPool, ObservedCounterIsComparedWithoutIncrement)
{
    pool_fixture fixture;
    const auto valid = fixture.sample();
    EXPECT_EQ(valid.nodes[0].counter, std::optional<uint32_t>{25});
    EXPECT_EQ(valid.nodes[0].reconstructed_handle, std::optional<uint32_t>{pool_fixture::parent});
    pool_fixture mismatch;
    mismatch.set<uint32_t>(pool_fixture::counter(0, pool_fixture::parent), 26);
    const auto invalid = mismatch.sample();
    EXPECT_EQ(invalid.nodes[0].reason, native_marker_pool_reason::generation_mismatch);
    EXPECT_EQ(invalid.visited_nodes, uint32_t{1});
    EXPECT_FALSE(invalid.conditional_stored_status);
}

TEST(NativeMarkerRequestPool, CounterMaskAndBothNativeWrapBranches)
{
    EXPECT_EQ(native_marker_pool_handle(25, 0x3c6, 0), uint32_t{0x0cf8c000});
    EXPECT_EQ(native_marker_pool_handle(0xff, 0x3ff, 0x1fff), uint32_t{0x7fffffff});
    EXPECT_EQ(native_marker_pool_handle(0, 0x40000000, 0), uint32_t{0x80000000});
    EXPECT_EQ(native_marker_pool_handle(7, 0x40003fff, 0x1fff), uint32_t{0xbfffffff});
    pool_fixture fixture;
    fixture.set<uint32_t>(pool_fixture::allocation(0) + 0x24, 0xf);
    const auto result = fixture.sample();
    EXPECT_EQ(result.nodes[0].counter_within_mask, std::optional<bool>{false});
    EXPECT_EQ(result.nodes[0].reason, native_marker_pool_reason::invalid_geometry);
}

TEST(NativeMarkerRequestPool, FailedPoisonedReadsNeverDecodePrefixes)
{
    const std::array<pool_read, 7> targets{{{pool_fixture::descriptor(pool_fixture::parent), 56},
                                            {pool_fixture::allocation(0), 56},
                                            {pool_fixture::inner(0), 32},
                                            {pool_fixture::counter(0, pool_fixture::parent), 4},
                                            {pool_fixture::free_link(0, pool_fixture::parent), 4},
                                            {pool_fixture::header(0, pool_fixture::parent) + 8, 8},
                                            {pool_fixture::header(0, pool_fixture::parent), 16}}};
    for (const auto target : targets)
    {
        pool_fixture fixture;
        fixture.failed = target;
        const auto result = fixture.sample();
        EXPECT_FALSE(result.nodes[0].best_effort_current_owner);
        EXPECT_FALSE(result.conditional_stored_status);
        EXPECT_FALSE(result.conditional_stored_detail);
        EXPECT_EQ(result.visited_nodes, uint32_t{1});
    }
}

TEST(NativeMarkerRequestPool, ChangedMetadataCounterHeaderAndRootsRejectContinuity)
{
    const std::array<pool_read, 10> targets{{{pool_fixture::descriptor(pool_fixture::parent), 56},
                                             {pool_fixture::allocation(0), 56},
                                             {pool_fixture::inner(0), 32},
                                             {pool_fixture::counter(0, pool_fixture::parent), 4},
                                             {pool_fixture::free_link(0, pool_fixture::parent), 4},
                                             {pool_fixture::header(0, pool_fixture::parent) + 8, 8},
                                             {pool_fixture::header(0, pool_fixture::parent), 16},
                                             {pool_fixture::base + 0x2439c70, 8},
                                             {pool_fixture::directory, 8},
                                             {pool_fixture::base + 0x1fb5f44, 12}}};
    for (const auto target : targets)
    {
        pool_fixture fixture;
        fixture.before_read = [&](const pool_read request, const uint32_t count) {
            if (request == target && (count == 2 || target.size == 12))
            {
                fixture.memory[request.address] ^= 1;
            }
        };
        const auto result = fixture.sample();
        EXPECT_FALSE(result.nodes[0].best_effort_current_owner);
        EXPECT_FALSE(result.conditional_stored_status);
        EXPECT_FALSE(result.conditional_stored_detail);
    }
}

TEST(NativeMarkerRequestPool, PassiveMetadataAndReadExceptionsRemainDistinct)
{
    pool_fixture rejected;
    rejected.rejected = {pool_fixture::base + 0x42c669, 69};
    const auto first = rejected.sample();
    EXPECT_EQ(first.resolver.raw.status, native_marker_read_status::passive_span_rejected);
    EXPECT_TRUE(rejected.reads.empty());
    pool_fixture metadata;
    metadata.metadata_throws = true;
    EXPECT_EQ(metadata.sample().resolver.raw.status, native_marker_read_status::metadata_exception);
    EXPECT_TRUE(metadata.reads.empty());
    pool_fixture reader;
    reader.reader_throws = true;
    const auto third = reader.sample();
    EXPECT_EQ(third.resolver.raw.status, native_marker_read_status::read_exception);
    EXPECT_FALSE(third.resolver.raw.bytes);
}

TEST(NativeMarkerRequestPool, InvalidImageAndMissingFullRootDoNotQueryMemory)
{
    pool_fixture image;
    EXPECT_EQ(image.sample(UINT64_MAX, 8).reason, native_marker_pool_reason::invalid_image);
    EXPECT_TRUE(image.guards.empty());
    pool_fixture root;
    root.resources.resource3.status = native_marker_read_status::read_failed;
    EXPECT_EQ(root.sample().reason, native_marker_pool_reason::resource_unavailable);
    EXPECT_TRUE(root.guards.empty());
    pool_fixture null;
    null.set<uint64_t>(pool_fixture::base + 0x2439c70, 0);
    EXPECT_EQ(null.sample().table.raw.status, native_marker_read_status::invalid_address);
}

TEST(NativeMarkerRequestPool, CheckedAddressArithmeticRejectsOverflowAndUnderflow)
{
    EXPECT_FALSE(native_marker_pool_add(UINT64_MAX, 1));
    pool_fixture table;
    table.set<uint64_t>(pool_fixture::directory, UINT64_MAX);
    const auto invalid = table.sample();
    EXPECT_EQ(invalid.nodes[0].initial.descriptor.raw.status, native_marker_read_status::invalid_address);
    pool_fixture view;
    view.set<uint32_t>(pool_fixture::descriptor(pool_fixture::parent) + 0x34, 0xffffffffU);
    view.set<uint64_t>(pool_fixture::header(0, pool_fixture::parent) + 8, UINT64_MAX);
    EXPECT_EQ(view.sample().nodes[0].reason, native_marker_pool_reason::invalid_geometry);
    pool_fixture counter;
    counter.set<uint64_t>(pool_fixture::allocation(0) + 8, UINT64_MAX);
    EXPECT_EQ(counter.sample().nodes[0].initial.counter.raw.status, native_marker_read_status::invalid_address);
}

TEST(NativeMarkerRequestPool, ExistingCoreBudgetIsPreservedUntilAllowance)
{
    native_marker_read_budget budget{1000};
    budget.reads = 23;
    budget.bytes = 196;
    EXPECT_FALSE(budget.try_reserve(1, 1000));
    EXPECT_EQ(budget.read_limit, uint32_t{23});
    EXPECT_EQ(budget.byte_limit, uint32_t{196});
    budget.allow_cleanup_pool();
    EXPECT_EQ(budget.read_limit, uint32_t{85});
    EXPECT_EQ(budget.byte_limit, uint32_t{1717});
    EXPECT_EQ(budget.started_ns, uint64_t{1000});
    EXPECT_TRUE(budget.read_exhausted);
}

TEST(NativeMarkerRequestPool, DeadlineAndBackwardClockStopBeforeMetadata)
{
    pool_fixture deadline;
    deadline.now += 2000000;
    const auto expired = deadline.sample();
    EXPECT_EQ(expired.resolver.raw.status, native_marker_read_status::time_limit);
    EXPECT_TRUE(deadline.guards.empty());
    EXPECT_EQ(deadline.budget.reads, uint32_t{23});
    pool_fixture backward;
    backward.now = 999;
    EXPECT_EQ(backward.sample().resolver.raw.status, native_marker_read_status::clock_invalid);
    EXPECT_TRUE(backward.guards.empty());
    pool_fixture throwing;
    throwing.clock_throws = true;
    EXPECT_EQ(throwing.sample().resolver.raw.status, native_marker_read_status::clock_invalid);
}

TEST(NativeMarkerRequestPool, CompletedLateReadKeepsRawAndLatchesExhaustion)
{
    pool_fixture fixture;
    fixture.advance_after_read = 2000000;
    const auto result = fixture.sample();
    EXPECT_TRUE(result.resolver.raw.bytes);
    EXPECT_EQ(result.resolver.raw.status, native_marker_read_status::complete);
    EXPECT_TRUE(fixture.budget.time_exhausted);
    EXPECT_EQ(result.reason, native_marker_pool_reason::time_or_clock_limit);
    EXPECT_EQ(result.reads, uint32_t{1});
    EXPECT_FALSE(result.directory.raw.bytes);
}

TEST(NativeMarkerRequestPool, ExpiryDuringMetadataPreventsBackendRead)
{
    pool_fixture fixture;
    fixture.advance_after_metadata = 2000000;
    const auto result = fixture.sample();
    EXPECT_EQ(result.resolver.raw.status, native_marker_read_status::time_limit);
    EXPECT_EQ(fixture.guards.size(), size_t{1});
    EXPECT_TRUE(fixture.reads.empty());
}

TEST(NativeMarkerRequestPool, KernelGuardFailureDoesNotRead)
{
    pool_fixture unlocked;
    unlocked.kernel.held = false;
    EXPECT_EQ(unlocked.sample().reason, native_marker_pool_reason::kernel_unavailable);
    EXPECT_TRUE(unlocked.guards.empty());
    pool_fixture throwing;
    throwing.kernel.throws = true;
    EXPECT_EQ(throwing.sample().reason, native_marker_pool_reason::kernel_unavailable);
    EXPECT_TRUE(throwing.guards.empty());
}

TEST(NativeMarkerRequestPool, BitmapIntervalBoundariesAreExplicitlyUnsupported)
{
    for (const uint32_t index : {uint32_t{1}, uint32_t{2}, uint32_t{3}, uint32_t{4}})
    {
        pool_fixture fixture;
        const auto handle = native_marker_pool_handle(25, 0x3c6, index);
        fixture.add_node(0, handle, pool_fixture::child, 2, 1, -3);
        for (size_t offset = 0; offset < 4; ++offset)
        {
            (*fixture.resources.resource3.bytes)[4 + offset] = static_cast<uint8_t>(handle >> (offset * 8));
            fixture.memory[pool_fixture::base + 0x1fb5f44 + 4 + offset] = static_cast<uint8_t>(handle >> (offset * 8));
        }
        fixture.set<uint16_t>(pool_fixture::inner(0) + 0x1a, 2);
        fixture.set<uint16_t>(pool_fixture::inner(0) + 0x1c, 4);
        const auto result = fixture.sample();
        const bool bitmap = index == 2 || index == 3;
        EXPECT_EQ(result.nodes[0].linked_mode, std::optional<bool>{!bitmap});
        if (bitmap)
        {
            EXPECT_EQ(result.nodes[0].reason, native_marker_pool_reason::unsupported_lifetime_mode);
            EXPECT_FALSE(result.nodes[0].free_tag);
            EXPECT_TRUE(result.nodes[0].initial.header.raw.bytes);
            EXPECT_TRUE(result.nodes[0].initial.counter.raw.bytes);
            EXPECT_TRUE(result.nodes[0].repeat_equal);
            EXPECT_EQ(result.nodes[0].related_handle, std::optional<uint32_t>{pool_fixture::child});
            EXPECT_FALSE(result.conditional_stored_status);
            EXPECT_EQ(result.visited_nodes, uint32_t{1});
        }
        else
        {
            EXPECT_TRUE(result.nodes[0].best_effort_current_owner);
        }
    }
}

TEST(NativeMarkerRequestPool, ChildRepeatFailureCannotReportAvailablePool)
{
    pool_fixture fixture;
    fixture.before_read = [&](const pool_read request, const uint32_t count) {
        if (request == pool_read{pool_fixture::header(1, pool_fixture::child), 16} && count == 2)
        {
            fixture.memory[request.address + 7] = 3;
        }
    };
    const auto result = fixture.sample();
    EXPECT_EQ(result.reason, native_marker_pool_reason::continuity_changed);
    EXPECT_FALSE(result.conditional_stored_status);
    EXPECT_FALSE(result.conditional_stored_detail);
}

TEST(NativeMarkerRequestPool, FinalClockGateRevokesQualificationButPreservesRaw)
{
    for (const uint32_t failure : {uint32_t{0}, uint32_t{1}, uint32_t{2}})
    {
        pool_fixture fixture;
        uint32_t final_read_clock_calls{};
        fixture.before_clock = [&] {
            if (!fixture.reads.empty() && fixture.reads.back() == pool_read{pool_fixture::base + 0x1fb5f44, 12})
            {
                if (++final_read_clock_calls == 2)
                {
                    if (failure == 0)
                    {
                        fixture.now += 2000000;
                    }
                    if (failure == 1)
                    {
                        fixture.now = 999;
                    }
                    if (failure == 2)
                    {
                        fixture.clock_throws = true;
                    }
                }
            }
        };
        const auto result = fixture.sample();
        EXPECT_EQ(result.reason, native_marker_pool_reason::time_or_clock_limit);
        EXPECT_FALSE(result.conditional_stored_status);
        EXPECT_FALSE(result.conditional_stored_detail);
        EXPECT_FALSE(result.status_node);
        EXPECT_FALSE(result.detail_node);
        EXPECT_FALSE(result.nodes[0].best_effort_current_owner);
        EXPECT_FALSE(result.nodes[1].best_effort_current_owner);
        EXPECT_TRUE(result.resource3_repeat.raw.bytes);
        EXPECT_TRUE(result.nodes[0].initial.header.raw.bytes);
    }
}

TEST(NativeMarkerRequestPool, EarlyFailureStillObservesFinalClock)
{
    pool_fixture fixture;
    fixture.resources.resource3.bytes.reset();
    fixture.now += 2000000;
    EXPECT_EQ(fixture.sample().reason, native_marker_pool_reason::time_or_clock_limit);
    EXPECT_TRUE(fixture.budget.time_exhausted);
    EXPECT_TRUE(fixture.reads.empty());
}

TEST(NativeMarkerRequestPool, NativeSlotWrapDiffersFromCounterProduct)
{
    pool_fixture fixture;
    const uint32_t handle = native_marker_pool_handle(25, 0x3c6, 2);
    fixture.add_node(0, handle, 0xffffffffU, 1, 2, 3);
    for (size_t offset = 0; offset < 4; ++offset)
    {
        (*fixture.resources.resource3.bytes)[4 + offset] = static_cast<uint8_t>(handle >> (offset * 8));
        fixture.memory[pool_fixture::base + 0x1fb5f44 + 4 + offset] = static_cast<uint8_t>(handle >> (offset * 8));
    }
    fixture.set<uint32_t>(pool_fixture::descriptor(handle) + 0x30, 0x80000000U);
    fixture.set<uint32_t>(pool_fixture::inner(0) + 0x10, 0x80000000U);
    fixture.set<uint32_t>(pool_fixture::allocation(0) + 0x20, 0x80000000U);
    fixture.set<uint32_t>(pool_fixture::storage(0) + 0x100000080ULL, 25);
    fixture.zero(pool_fixture::data(0), 16);
    fixture.memory[pool_fixture::data(0) + 6] = 1;
    fixture.memory[pool_fixture::data(0) + 7] = 2;
    fixture.set<uint32_t>(pool_fixture::data(0) + 0xc, 0xffffffffU);
    fixture.set<uint32_t>(pool_fixture::free_data(0) + 0x40, 0);
    const auto result = fixture.sample();
    EXPECT_EQ(result.nodes[0].initial.header.address, pool_fixture::data(0));
    EXPECT_EQ(result.nodes[0].initial.free_link.address, pool_fixture::free_data(0) + 0x40);
    EXPECT_EQ(result.nodes[0].initial.counter.address, pool_fixture::storage(0) + 0x100000080ULL);
    EXPECT_TRUE(result.nodes[0].best_effort_current_owner);
}

TEST(NativeMarkerRequestPool, NegativeFullHandleUsesBit30BranchAndConfirmedZeroFields)
{
    pool_fixture fixture;
    constexpr uint32_t handle = 0x80000000U;
    fixture.add_node(0, handle, 0xffffffffU, 1, 0, 0);
    fixture.set<uint32_t>(pool_fixture::allocation(0) + 0x34, 0x40000000U);
    fixture.set<uint32_t>(pool_fixture::allocation(0) + 0x24, 7);
    fixture.set<uint32_t>(pool_fixture::counter(0, handle), 0);
    for (size_t offset = 0; offset < 4; ++offset)
    {
        (*fixture.resources.resource3.bytes)[4 + offset] = static_cast<uint8_t>(handle >> (offset * 8));
        fixture.memory[pool_fixture::base + 0x1fb5f44 + 4 + offset] = static_cast<uint8_t>(handle >> (offset * 8));
    }
    const auto result = fixture.sample();
    EXPECT_EQ(result.nodes[0].group, uint32_t{0});
    EXPECT_EQ(result.nodes[0].reconstructed_handle, std::optional<uint32_t>{0x80000000U});
    EXPECT_EQ(result.nodes[0].counter, std::optional<uint32_t>{0});
    EXPECT_EQ(result.conditional_stored_status, std::optional<int8_t>{0});
    EXPECT_EQ(result.conditional_stored_detail, std::optional<int8_t>{0});
    EXPECT_TRUE(result.nodes[0].best_effort_current_owner);
}

TEST(NativeMarkerRequestPool, NegativeRelocationMaskPreservesFull64BitDisplacement)
{
    pool_fixture fixture;
    fixture.set<uint64_t>(pool_fixture::descriptor(pool_fixture::parent) + 8, 0x125000000ULL);
    fixture.set<uint32_t>(pool_fixture::descriptor(pool_fixture::parent) + 0x34, 0xffffffffU);
    fixture.set<uint64_t>(0x125000008ULL, 0x100000000ULL);
    const auto result = fixture.sample();
    EXPECT_EQ(result.nodes[0].initial.encoded.address, uint64_t{0x125000008ULL});
    EXPECT_EQ(result.nodes[0].initial.header.address, uint64_t{0x25000000ULL});
    EXPECT_TRUE(result.nodes[0].best_effort_current_owner);
    EXPECT_EQ(result.nodes[0].handle, std::optional<uint32_t>{0x0cf8c000U});
}

TEST(NativeMarkerRequestPool, SameLowIndexWithDifferentGenerationIsNotAHandleCycle)
{
    pool_fixture fixture;
    constexpr uint32_t next_generation = 0x0d78c000U;
    fixture.set<uint32_t>(pool_fixture::header(0, pool_fixture::parent) + 0xc, next_generation);
    const auto result = fixture.sample();
    EXPECT_EQ(result.visited_nodes, uint32_t{2});
    EXPECT_EQ(result.nodes[1].handle, std::optional<uint32_t>{next_generation});
    EXPECT_EQ(result.nodes[1].reason, native_marker_pool_reason::generation_mismatch);
    EXPECT_NE(result.traversal_reason, native_marker_pool_reason::cycle);
}

TEST(NativeMarkerRequestPool, LeaseAndIdentityRejectionRetainRawButClearPoolInterpretations)
{
    for (const bool change_identity : {false, true})
    {
        native_marker_capture_state state;
        native_marker_identity identity{77, 123, 9, pool_fixture::base, pool_fixture::image_size};
        ASSERT_TRUE(state.arm("1", identity, 1000, 1500));
        const auto entry = state.observe(identity, native_marker_capture_state::investment_marker, 1000);
        ASSERT_TRUE(entry);
        ASSERT_TRUE(state.finish(identity, entry.sequence, true, 1100).accepted);
        const auto cleanup = state.observe(identity, native_marker_capture_state::cleanup_marker, 1400);
        ASSERT_TRUE(cleanup);
        pool_fixture fixture;
        fixture.now = 1400;
        fixture.budget.started_ns = 1400;
        auto pool = fixture.sample();
        const auto raw_header = pool.nodes[0].initial.header.raw.bytes;
        const auto raw_counter = pool.nodes[0].initial.counter.raw.bytes;
        ASSERT_TRUE(pool.conditional_stored_status);
        ASSERT_TRUE(pool.nodes[0].best_effort_current_owner);
        if (change_identity)
        {
            ++identity.birth;
        }
        const auto completion = state.finish(identity, cleanup.sequence, true, change_identity ? 1450 : 1600);
        ASSERT_FALSE(completion.accepted);
        EXPECT_EQ(completion.reason, change_identity ? native_marker_stop::identity_changed : native_marker_stop::expired);
        EXPECT_FALSE(fixture.budget.time_exhausted);
        native_marker_pool_unqualify(pool, native_marker_pool_reason::capture_rejected);
        EXPECT_EQ(pool.reason, native_marker_pool_reason::capture_rejected);
        EXPECT_EQ(pool.nodes[0].initial.header.raw.bytes, raw_header);
        EXPECT_EQ(pool.nodes[0].initial.counter.raw.bytes, raw_counter);
        EXPECT_TRUE(pool.nodes[0].generation_bits_match);
        EXPECT_FALSE(pool.nodes[0].best_effort_current_owner);
        EXPECT_FALSE(pool.nodes[1].best_effort_current_owner);
        EXPECT_FALSE(pool.conditional_stored_status);
        EXPECT_FALSE(pool.conditional_stored_detail);
        EXPECT_FALSE(pool.status_node);
        EXPECT_FALSE(pool.detail_node);
    }
}

TEST(NativeMarkerRequestPool, CycleAndDepthBoundSuppressTerminalParentStatus)
{
    for (const bool cycle : {false, true})
    {
        pool_fixture fixture;
        fixture.memory[pool_fixture::header(0, pool_fixture::parent) + 7] = 2;
        if (cycle)
        {
            fixture.add_node(1, pool_fixture::child, pool_fixture::parent, 2, 1, 4);
        }
        else
        {
            const uint32_t third = native_marker_pool_handle(7, 0x3c8, 2);
            const uint32_t fourth = native_marker_pool_handle(8, 0x3c9, 3);
            const uint32_t fifth = native_marker_pool_handle(9, 0x3ca, 4);
            fixture.add_node(1, pool_fixture::child, third, 2, 1, 4);
            fixture.add_node(2, third, fourth, 2, 1, 5);
            fixture.add_node(3, fourth, fifth, 2, 1, 6);
        }
        const auto result = fixture.sample();
        EXPECT_EQ(result.nodes[0].stored_status, std::optional<int8_t>{2});
        EXPECT_EQ(result.reason, cycle ? native_marker_pool_reason::cycle : native_marker_pool_reason::traversal_required);
        EXPECT_FALSE(result.conditional_stored_status);
        EXPECT_FALSE(result.conditional_stored_detail);
        EXPECT_FALSE(result.status_node);
        EXPECT_FALSE(result.detail_node);
    }
}

TEST(NativeChildSample, PostPollUsesNativeEaxAndFreshContextWithExactReadBounds)
{
    pool_fixture fixture;
    const auto selected = fixture.select_child();
    const auto registers = fixture.child_registers(selected, 0);
    const auto result = fixture.sample_child(selected, 0, registers);
    ASSERT_TRUE(result.child_qualified);
    EXPECT_TRUE(result.resource_identity);
    EXPECT_EQ(result.raw_return, std::optional<uint32_t>{0x19});
    EXPECT_EQ(result.context_word, std::optional<uint16_t>{0x1234});
    EXPECT_EQ(result.pool->nodes[1].stored_status, std::optional<int8_t>{1});
    EXPECT_EQ(result.budget.reads, 39U);
    EXPECT_EQ(result.budget.bytes, 841U);
    EXPECT_EQ(fixture.reads.size(), 39U);
    EXPECT_EQ(fixture.reads, fixture.guards);
    EXPECT_FALSE(result.pool->strong_lifetime);
    EXPECT_FALSE(result.effective_status);
    EXPECT_FALSE(result.effective_terminal);
}

TEST(NativeChildSample, PreReleasePreservesEverySignedStatusAndOnlyGreaterThanOnePredicate)
{
    for (const int32_t status : {-69, -1, 0, 1, 2, 5})
    {
        pool_fixture fixture;
        const auto selected = fixture.select_child();
        const auto registers = fixture.child_registers(selected, 1, status);
        const auto result = fixture.sample_child(selected, 1, registers);
        ASSERT_TRUE(result.resource_identity);
        ASSERT_TRUE(result.child_qualified);
        EXPECT_EQ(result.kind, native_child_kind::pre_release);
        EXPECT_EQ(result.effective_status, std::optional<int32_t>{status});
        EXPECT_EQ(result.detail_return, std::optional<uint8_t>{0x7f});
        EXPECT_EQ(result.slot_detail, std::optional<uint8_t>{0});
        EXPECT_EQ(result.effective_terminal, status > 1);
        EXPECT_EQ(result.budget.reads, 35U);
        EXPECT_EQ(result.budget.bytes, 829U);
        EXPECT_FALSE(result.raw_return);
        EXPECT_FALSE(result.context_word);
    }
}

TEST(NativeChildSample, PreReleaseIdentitySurvivesOptionalChildReadFailure)
{
    pool_fixture fixture;
    const auto selected = fixture.select_child();
    const auto registers = fixture.child_registers(selected, 1, 1);
    fixture.failed = {selected.child_view, 16};
    const auto result = fixture.sample_child(selected, 1, registers);
    EXPECT_TRUE(result.resource_identity);
    EXPECT_FALSE(result.child_qualified);
    EXPECT_EQ(result.effective_status, std::optional<int32_t>{1});
    EXPECT_FALSE(result.effective_terminal);
    EXPECT_LE(result.budget.reads, 35U);
    EXPECT_LE(result.budget.bytes, 829U);
}

TEST(NativeChildSample, ForeignNativeViewOrIncompleteRegistersCauseNoGuestReads)
{
    for (const bool incomplete : {false, true})
    {
        pool_fixture fixture;
        const auto selected = fixture.select_child();
        auto registers = fixture.child_registers(selected, 0);
        if (incomplete)
        {
            registers.context.available[0] &= ~(uint64_t{1} << 15);
        }
        else
        {
            ++registers.context.gpr[5];
        }
        const auto result = fixture.sample_child(selected, 0, registers);
        EXPECT_FALSE(result.child_qualified);
        EXPECT_EQ(result.reason, native_child_reason::native_view_mismatch);
        EXPECT_TRUE(fixture.reads.empty());
    }
}

TEST(NativeChildSample, ActiveHandleAndContextRepeatMutationRejectJoin)
{
    for (const bool active : {false, true})
    {
        pool_fixture fixture;
        const auto selected = fixture.select_child();
        const auto registers = fixture.child_registers(selected, 0);
        const auto address = active ? registers.context.gpr[15] + 0x220068 : registers.context.gpr[1];
        fixture.before_read = [&](const pool_read read, const uint32_t count) {
            if (read.address == address && count == 2)
            {
                ++fixture.memory[address];
            }
        };
        const auto result = fixture.sample_child(selected, 0, registers);
        EXPECT_FALSE(result.child_qualified);
        EXPECT_EQ(result.reason, native_child_reason::continuity_changed);
        EXPECT_EQ(result.raw_return, std::optional<uint32_t>{0x19});
        EXPECT_FALSE(result.context_word);
    }
}

TEST(NativeChildSample, ContextSentinelAndReusedPoolGenerationRejectJoin)
{
    for (const bool sentinel : {false, true})
    {
        pool_fixture fixture;
        const auto selected = fixture.select_child();
        const auto registers = fixture.child_registers(selected, 0);
        if (sentinel)
        {
            fixture.set<uint16_t>(registers.context.gpr[1], 0xffff);
        }
        else
        {
            fixture.set<uint32_t>(pool_fixture::counter(1, pool_fixture::child), selected.counters[1] + 1);
        }
        const auto result = fixture.sample_child(selected, 0, registers);
        EXPECT_FALSE(result.child_qualified);
        EXPECT_EQ(result.raw_return, std::optional<uint32_t>{0x19});
    }
}

TEST(NativeChildSample, FinalClockFailurePreservesRawAndRevokesDerivedQualification)
{
    pool_fixture fixture;
    const auto selected = fixture.select_child();
    const auto registers = fixture.child_registers(selected, 0);
    fixture.before_read = [&](const pool_read read, const uint32_t count) {
        if (read.address == registers.context.gpr[1] && count == 2)
        {
            fixture.now += native_marker_read_budget::max_elapsed_ns;
        }
    };
    const auto result = fixture.sample_child(selected, 0, registers);
    EXPECT_FALSE(result.child_qualified);
    EXPECT_FALSE(result.resource_identity);
    EXPECT_FALSE(result.clock_accepted);
    EXPECT_FALSE(result.context_word);
    EXPECT_EQ(result.raw_return, std::optional<uint32_t>{0x19});
    EXPECT_TRUE(result.active_initial.raw.bytes);
    ASSERT_TRUE(result.pool);
    EXPECT_FALSE(result.pool->nodes[0].best_effort_current_owner);
}

TEST(NativeChildSample, KernelAndClockExceptionsStayBoundedAndUnavailable)
{
    for (const bool clock : {false, true})
    {
        pool_fixture fixture;
        const auto selected = fixture.select_child();
        const auto registers = fixture.child_registers(selected, 0);
        fixture.clock_throws = clock;
        fixture.kernel.throws = !clock;
        const auto result = fixture.sample_child(selected, 0, registers);
        EXPECT_FALSE(result.child_qualified);
        EXPECT_FALSE(result.clock_accepted);
        EXPECT_TRUE(fixture.reads.empty());
        EXPECT_LE(result.budget.reads, 39U);
        EXPECT_LE(result.budget.bytes, 841U);
    }
}

TEST(NativeChildSample, SelectionRejectsNonreciprocalOrNonType3Child)
{
    for (const bool wrong_type : {false, true})
    {
        pool_fixture fixture;
        fixture.add_node(1, pool_fixture::child, wrong_type ? pool_fixture::parent : 0xffffffffU, wrong_type ? 1 : 3, 1, 0);
        native_marker_snapshot marker{};
        marker.identity = {1, 2, 3, pool_fixture::base, pool_fixture::image_size};
        marker.reservation = {native_marker_event::cleanup_entry, 2, fixture.now};
        marker.complete = marker.core_complete = true;
        marker.resources = fixture.resources;
        marker.request_pool = std::make_shared<const native_marker_pool_sample>(fixture.sample());
        EXPECT_FALSE(select_native_child(marker));
    }
}

TEST(NativeChildReporter, LifecycleReserveFitsMaximumWidthNormalSevenRecordPath)
{
    native_child_sample sample{};
    sample.reason = native_child_reason::removed;
    sample.site = UINT32_MAX;
    sample.sequence = UINT64_MAX;
    sample.began_ns = UINT64_MAX;
    sample.ended_ns = UINT64_MAX;
    sample.collection_ns = UINT64_MAX;
    sample.callback_ns = UINT64_MAX;
    sample.output_bytes = UINT64_MAX;
    sample.output_rejections = UINT64_MAX;
    sample.removal_accepted = false;
    sample.selection.identity = {UINT32_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    sample.selection.marker_sequence = UINT64_MAX;
    sample.selection.selected_ns = UINT64_MAX;
    sample.selection.parent_handle = UINT32_MAX;
    sample.selection.child_handle = UINT32_MAX;
    sample.selection.tag = UINT32_MAX;
    sample.selection.parent_view = UINT64_MAX;
    sample.selection.child_view = UINT64_MAX;
    sample.selection.counters.fill(UINT32_MAX);
    sample.selection.configs.fill(UINT32_MAX);
    for (uint32_t site = 0; site < 2; ++site)
    {
        auto& state = sample.sites[site];
        state.raw_traps = UINT64_MAX;
        state.active_traps = native_child_limits::trap_caps[site];
        state.retired_traps = UINT64_MAX;
        state.filtered = native_child_limits::trap_caps[site];
        state.attempts = native_child_limits::attempts_per_site;
        state.accepted = 1;
        state.installed_ns = UINT64_MAX;
        state.retired_ns = UINT64_MAX;
        state.removed_ns = UINT64_MAX;
        state.removal_attempts = UINT64_MAX;
        state.removal_deferrals = UINT64_MAX;
        state.retirement = native_child_reason::removed;
    }
    sample.budget.reads = 3;
    sample.budget.bytes = 130;
    sample.resolver_guard[0] = {native_marker_pool_resolver_bytes, native_marker_read_status::complete};
    sample.poll_guard = {native_child_poll_guard, native_marker_read_status::complete};
    sample.release_guard = {native_child_release_guard, native_marker_read_status::complete};
    size_t charged{};
    for (const auto kind :
         {native_child_kind::selection, native_child_kind::activation, native_child_kind::coverage, native_child_kind::removed,
          native_child_kind::coverage, native_child_kind::removed, native_child_kind::coverage})
    {
        sample.kind = kind;
        sogen::debug_string_event event{};
        event.header.instruction_count = UINT64_MAX;
        event.transport = "native_child_probe";
        event.details = "Bounded native resource3 child observation";
        event.native_child_capture = std::make_shared<const native_child_sample>(sample);
        charged += sogen::serialized_analysis_event_bytes(event);
    }
    EXPECT_LE(charged, native_child_limits::lifecycle_bytes);
}

namespace sogen
{
    struct native_child_capture_test_access
    {
        using dependencies = native_child_capture::dependencies;

        static void complete_synthetic_setup(windows_emulator& win, mapped_module& executable)
        {
            if (win.setup_completed_ || win.mod_manager.executable || !executable.imports.empty() ||
                win.mod_manager.find_by_address(executable.image_base) != &executable)
            {
                throw std::logic_error("fixture synthetic setup prerequisites");
            }
            win.mod_manager.executable = &executable;
            win.setup_completed_ = true;
        }

        struct state
        {
            bool stopped{};
            bool present{};
            bool reporting_stopped{};
            size_t owned_hooks{};
            detail::native_child_selection selection{};
            std::array<detail::native_child_site_counts, 2> sites{};
            uint64_t collection_ns{};
            uint64_t callback_ns{};
        };

        static void observe(windows_emulator& win, const detail::native_marker_snapshot& marker, dependencies operations)
        {
            if (!win.native_child_capture_stopped_ && !win.native_child_capture_ && native_child_capture::enabled() &&
                marker.reservation.event == detail::native_marker_event::cleanup_entry && win.has_active_dispatch_context())
            {
                win.native_child_capture_ = native_child_capture::create(win, win.kernel_lock_, marker, std::move(operations));
            }
        }

        static state inspect(windows_emulator& win)
        {
            state result{};
            win.synchronize_native_child_capture([&] {
                result.stopped = win.native_child_capture_stopped_;
                if (const auto capture = win.native_child_capture_)
                {
                    result.present = true;
                    result.reporting_stopped = capture->reporting_stopped();
                    result.owned_hooks =
                        static_cast<size_t>(capture->hooks_[0] != nullptr) + static_cast<size_t>(capture->hooks_[1] != nullptr);
                    result.selection = capture->selected_;
                    result.sites = capture->counts_;
                    result.collection_ns = capture->collection_ns_;
                    result.callback_ns = capture->callback_ns_;
                }
            });
            return result;
        }

        static void flush(windows_emulator& win, const std::function<void()>& operation, const std::function<void()>& on_wait)
        {
            win.synchronize_native_child_capture_impl(operation, false, on_wait);
        }

        static bool shutdown(windows_emulator& win, const std::function<void()>& on_wait)
        {
            return win.shutdown_native_child_capture_impl(on_wait);
        }
    };
}

namespace
{
    class child_test_environment
    {
      public:
        child_test_environment(const char* name, const char* value)
            : name_(name)
        {
            if (const auto* previous = std::getenv(name))
            {
                previous_ = previous;
            }
            set(value);
        }

        ~child_test_environment()
        {
            set(previous_ ? previous_->c_str() : nullptr);
        }

      private:
        void set(const char* value)
        {
#ifdef _WIN32
            _putenv_s(name_, value ? value : "");
#else
            if (value)
            {
                setenv(name_, value, 1);
            }
            else
            {
                unsetenv(name_);
            }
#endif
        }

        const char* name_{};
        std::optional<std::string> previous_{};
    };

    struct child_lifecycle_environment
    {
        child_test_environment enabled{"SOGEN_DESTINY_CHILD_CAPTURE", "1"};
        child_test_environment startup{"SOGEN_DESTINY_STARTUP_CAPTURE", "0"};
        child_test_environment reason175{"SOGEN_DESTINY_REASON175_CAPTURE", "0"};
    };

    enum class child_failure
    {
        none,
        worker_start,
        first_install,
        second_install,
        guard,
        selection_report
    };

    struct child_record
    {
        uint32_t attempt{};
        size_t serialized_bytes{};
        std::shared_ptr<const native_child_sample> sample{};
    };

    struct child_callback_counts
    {
        uint32_t attempts{}, charges{}, exceptions{}, zero_returns{};
        uint64_t charged_bytes{};
    };

    struct child_reporter_state
    {
        std::atomic<const char*> phase{"setup"};
        std::atomic<bool> destroyed{}, throw_child{}, throw_flush{}, block_coverage{};
        std::atomic<uint32_t> after_destroy{}, inside{}, overlaps{}, intentional_overlaps{}, flushes{}, child_throws{}, flush_throws{};
        std::atomic<uint32_t> callback_attempts{}, callback_exceptions{}, callback_charges{}, callback_zero_returns{}, active_attempt{},
            injected_failures{};
        std::atomic<uint32_t> worker_attempts{}, worker_entries{}, worker_stop_requests{}, worker_exits{}, hook_attempts{},
            actual_installs{};
        std::atomic<uint32_t> kernel_waits{}, external_entries{}, external_joins{}, gate_timeouts{}, guard_reads{}, guard_bytes{};
        std::atomic<uint64_t> charged_bytes{};
        std::mutex mutex{};
        std::condition_variable condition{};
        bool coverage_entered{}, release_coverage{};
        std::vector<std::string> order{};
        std::vector<child_record> records{};
        std::map<uint32_t, child_callback_counts> callback_counts{};
    };

    class child_fixture_watchdog
    {
      public:
        explicit child_fixture_watchdog(std::shared_ptr<child_reporter_state> state)
            : worker_([state = std::move(state)](const std::stop_token stop) {
                  std::mutex mutex;
                  std::condition_variable condition;
                  const std::stop_callback notify(stop, [&] {
                      const std::lock_guard guard(mutex);
                      condition.notify_all();
                  });
                  std::unique_lock guard(mutex);
                  if (!condition.wait_for(guard, std::chrono::seconds(20), [&] { return stop.stop_requested(); }))
                  {
                      std::fprintf(stderr, "CHILD_FIXTURE_WATCHDOG_FAILURE phase=%s\n", state->phase.load());
                      std::fflush(stderr);
                      std::_Exit(86);
                  }
              })
        {
        }

      private:
        std::jthread worker_;
    };

    void release_child_reporter(const std::shared_ptr<child_reporter_state>& state)
    {
        {
            const std::lock_guard guard(state->mutex);
            state->release_coverage = true;
        }
        state->condition.notify_all();
    }

    class child_external_operation
    {
      public:
        child_external_operation(std::shared_ptr<child_reporter_state> state, std::function<void()> operation)
            : state_(std::move(state)),
              completed_(promise_.get_future()),
              worker_([this, operation = std::move(operation)] {
                  ++state_->external_entries;
                  try
                  {
                      operation();
                  }
                  catch (...)
                  {
                      error_ = std::current_exception();
                  }
                  promise_.set_value();
              })
        {
        }

        ~child_external_operation()
        {
            release_child_reporter(state_);
            join();
        }

        bool completed(const std::chrono::milliseconds timeout)
        {
            return completed_.wait_for(timeout) == std::future_status::ready;
        }

        void join()
        {
            if (!worker_.joinable())
            {
                return;
            }
            if (!completed(std::chrono::seconds(5)))
            {
                std::fprintf(stderr, "CHILD_FIXTURE_EXTERNAL_OPERATION_FAILURE phase=%s\n", state_->phase.load());
                std::fflush(stderr);
                std::_Exit(86);
            }
            worker_.join();
            ++state_->external_joins;
        }

        void rethrow() const
        {
            if (error_)
            {
                std::rethrow_exception(error_);
            }
        }

      private:
        std::shared_ptr<child_reporter_state> state_;
        std::promise<void> promise_;
        std::future<void> completed_;
        std::exception_ptr error_;
        std::thread worker_;
    };

    std::vector<child_record> snapshot_child_records(const std::shared_ptr<child_reporter_state>& state)
    {
        const std::lock_guard guard(state->mutex);
        return state->records;
    }

    class child_lifecycle_reporter final : public sogen::analysis_reporter
    {
      public:
        explicit child_lifecycle_reporter(std::shared_ptr<child_reporter_state> state)
            : state_(std::move(state))
        {
        }

        ~child_lifecycle_reporter() override
        {
            EXPECT_EQ(state_->inside.load(), 0U);
            state_->destroyed.store(true);
        }

        void report(const sogen::analysis_event& event) override
        {
            enter();
            const auto leave = sogen::utils::finally([&] { --state_->inside; });
            if (const auto* debug = std::get_if<sogen::debug_string_event>(&event); debug && debug->native_child_capture)
            {
                const auto& child = debug->native_child_capture;
                {
                    const std::lock_guard guard(state_->mutex);
                    state_->order.emplace_back("child");
                    state_->records.push_back({state_->active_attempt.load(), sogen::serialized_analysis_event_bytes(event), child});
                }
                if (child->kind == native_child_kind::coverage && state_->block_coverage.exchange(false))
                {
                    state_->phase.store("blocked_child_report");
                    std::unique_lock guard(state_->mutex);
                    state_->coverage_entered = true;
                    state_->condition.notify_all();
                    if (!state_->condition.wait_for(guard, std::chrono::seconds(5), [&] { return state_->release_coverage; }))
                    {
                        ++state_->gate_timeouts;
                        ADD_FAILURE() << "Independent reporter release did not arrive";
                    }
                }
                if (state_->throw_child.load())
                {
                    ++state_->child_throws;
                    throw std::runtime_error("fixture_child_throw");
                }
            }
            else if (std::holds_alternative<sogen::run_finished_event>(event) || std::holds_alternative<sogen::run_failed_event>(event))
            {
                const std::lock_guard guard(state_->mutex);
                state_->order.emplace_back(std::holds_alternative<sogen::run_failed_event>(event) ? "failed" : "finished");
            }
        }

        void flush() override
        {
            enter();
            const auto leave = sogen::utils::finally([&] { --state_->inside; });
            ++state_->flushes;
            if (state_->throw_flush.load())
            {
                ++state_->flush_throws;
                throw std::runtime_error("fixture_flush_throw");
            }
            const std::lock_guard guard(state_->mutex);
            state_->order.emplace_back("flush");
        }

      private:
        void enter()
        {
            if (state_->destroyed.load())
            {
                ++state_->after_destroy;
            }
            if (state_->inside.fetch_add(1))
            {
                ++state_->overlaps;
            }
        }

        std::shared_ptr<child_reporter_state> state_;
    };

    std::unique_ptr<sogen::windows_emulator> child_lifecycle_emulator()
    {
        sogen::emulator_settings settings{.disable_logging = true, .use_instruction_precision = false};
        settings.load_registry = false;
        settings.emulation_root = sogen::test::get_emulator_root();
        sogen::emulator_interfaces interfaces{};
        interfaces.ui = std::make_unique<sogen::null_ui_backend>();
        auto win = std::make_unique<sogen::windows_emulator>(sogen::icicle::create_x86_64_emulator(), settings, sogen::emulator_callbacks{},
                                                             std::move(interfaces));
        const auto allocate = [&](const uint64_t page) {
            if (!win->memory.allocate_memory(page, 0x1000, sogen::memory_permission::read_write))
            {
                throw std::runtime_error("fixture page allocation");
            }
        };
        allocate(pool_fixture::base);
        sogen::PEDosHeader_t dos{};
        dos.e_magic = 0x5A4D;
        dos.e_lfanew = 0x80;
        win->memory.write_memory(pool_fixture::base, &dos, sizeof(dos));
        sogen::PENTHeaders_t<uint64_t> nt{};
        nt.Signature = 0x4550;
        nt.FileHeader.Machine = sogen::PEMachineType::AMD64;
        nt.FileHeader.SizeOfOptionalHeader = static_cast<uint16_t>(sizeof(nt.OptionalHeader));
        nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE;
        nt.OptionalHeader.Magic = 0x20B;
        nt.OptionalHeader.ImageBase = pool_fixture::base;
        nt.OptionalHeader.SizeOfImage = 0x1000;
        nt.OptionalHeader.SizeOfHeaders = 0x1000;
        nt.OptionalHeader.SectionAlignment = 0x1000;
        nt.OptionalHeader.FileAlignment = 0x200;
        nt.OptionalHeader.NumberOfRvaAndSizes = 16;
        win->memory.write_memory(pool_fixture::base + 0x80, &nt, sizeof(nt));
        auto* module = win->mod_manager.map_memory_module(pool_fixture::base, 0x1000, R"(C:\destiny2.exe)", win->log);
        if (!module)
        {
            throw std::runtime_error("fixture module mapping");
        }
        module->size_of_image = pool_fixture::image_size;
        for (const auto page : {pool_fixture::base + 0x42C000, pool_fixture::base + 0x43C000, pool_fixture::base + 0xB44000})
        {
            allocate(page);
        }
        win->memory.write_memory(pool_fixture::base + 0x42C669, native_marker_pool_resolver_bytes.data(),
                                 native_marker_pool_resolver_bytes.size());
        win->memory.write_memory(pool_fixture::base + 0x43C5D9, native_child_poll_guard.data(), native_child_poll_guard.size());
        win->memory.write_memory(pool_fixture::base + 0xB440A5, native_child_release_guard.data(), native_child_release_guard.size());
        sogen::native_child_capture_test_access::complete_synthetic_setup(*win, *module);
        return win;
    }

    testing::AssertionResult arm_child_lifecycle(sogen::windows_emulator& win, sogen::analysis_context& context,
                                                 const std::shared_ptr<child_reporter_state>& state,
                                                 const child_failure failure = child_failure::none)
    {
        if (sogen::native_child_capture_test_access::inspect(win).stopped)
        {
            return testing::AssertionFailure() << "capture owner is permanently stopped";
        }
        ++state->active_attempt;
        state->phase.store("arm");
        sogen::register_analysis_callbacks(context);
        const auto original = win.callbacks.on_native_child_capture;
        win.callbacks.on_native_child_capture = [original, state](std::shared_ptr<const native_child_sample> sample,
                                                                  const size_t maximum_bytes) {
            const auto attempt = state->active_attempt.load();
            {
                const std::lock_guard guard(state->mutex);
                ++state->callback_counts[attempt].attempts;
            }
            ++state->callback_attempts;
            if (state->destroyed.load())
            {
                ++state->after_destroy;
                ++state->callback_zero_returns;
                const std::lock_guard guard(state->mutex);
                ++state->callback_counts[attempt].zero_returns;
                return size_t{0};
            }
            try
            {
                const auto charged = original(std::move(sample), maximum_bytes);
                if (charged)
                {
                    ++state->callback_charges;
                    state->charged_bytes.fetch_add(charged);
                    const std::lock_guard guard(state->mutex);
                    auto& counts = state->callback_counts[attempt];
                    ++counts.charges;
                    counts.charged_bytes += charged;
                }
                else
                {
                    ++state->callback_zero_returns;
                    const std::lock_guard guard(state->mutex);
                    ++state->callback_counts[attempt].zero_returns;
                }
                return charged;
            }
            catch (...)
            {
                ++state->callback_exceptions;
                const std::lock_guard guard(state->mutex);
                ++state->callback_counts[attempt].exceptions;
                throw;
            }
        };
        sogen::native_child_capture_test_access::dependencies operations;
        operations.start_worker = [state, failure](std::function<void(std::stop_token)> body) {
            ++state->worker_attempts;
            if (failure == child_failure::worker_start)
            {
                ++state->injected_failures;
                throw std::runtime_error("fixture_worker_start");
            }
            return std::jthread([state, body = std::move(body)](const std::stop_token stop) {
                ++state->worker_entries;
                const std::stop_callback stopped(stop, [state] { ++state->worker_stop_requests; });
                const auto exit = sogen::utils::finally([&] { ++state->worker_exits; });
                body(stop);
            });
        };
        operations.install_hook = [&win, state, failure](const uint32_t site, const uint64_t address,
                                                         const sogen::hook_interface::memory_execution_hook_mode mode,
                                                         sogen::memory_execution_hook_callback callback) {
            ++state->hook_attempts;
            if ((site == 0 && failure == child_failure::first_install) || (site == 1 && failure == child_failure::second_install))
            {
                ++state->injected_failures;
                throw std::runtime_error("fixture_hook_install");
            }
            auto* hook = win.emu().hook_memory_execution_with_mode(address, mode, std::move(callback));
            ++state->actual_installs;
            return hook;
        };
        pool_fixture pool;
        pool.add_node(1, pool_fixture::child, pool_fixture::parent, 3, 1, 0);
        native_marker_snapshot marker{};
        marker.identity = {1, 2, 3, pool_fixture::base, pool_fixture::image_size};
        marker.complete = marker.core_complete = true;
        marker.resources = pool.resources;
        marker.request_pool = std::make_shared<const native_marker_pool_sample>(pool.sample());
        marker.reservation = {
            native_marker_event::cleanup_entry, 2,
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())};
        try
        {
            win.dispatch_on_cpu(win.vcpu(0).cpu,
                                [&] { sogen::native_child_capture_test_access::observe(win, marker, std::move(operations)); });
        }
        catch (const std::exception& error)
        {
            return testing::AssertionFailure() << "arm exception: " << error.what();
        }
        const auto current = sogen::native_child_capture_test_access::inspect(win);
        if (!current.present || current.owned_hooks != 2)
        {
            return testing::AssertionFailure() << "arm lacks two owned hook API handles";
        }
        const auto records = snapshot_child_records(state);
        if (records.size() < 2)
        {
            return testing::AssertionFailure() << "arm lacks selection/activation records";
        }
        if (records.back().sample->kind != native_child_kind::activation || records.back().sample->reason != native_child_reason::available)
        {
            return testing::AssertionFailure() << "arm activation was rejected";
        }
        return testing::AssertionSuccess();
    }

    void flush_child_lifecycle(sogen::windows_emulator& win, const sogen::analysis_context& context)
    {
        win.synchronize_native_child_capture([&] {
            for (auto* reporter : context.reporters)
            {
                reporter->flush();
            }
        });
    }

    void check_child_original_bytes(sogen::windows_emulator& win, const std::shared_ptr<child_reporter_state>& state)
    {
        std::array<uint8_t, 7> poll{};
        std::array<uint8_t, 5> release{};
        ++state->guard_reads;
        state->guard_bytes.fetch_add(static_cast<uint32_t>(poll.size()));
        EXPECT_TRUE(win.memory.try_read_memory(pool_fixture::base + 0x43C5E1, poll.data(), poll.size()));
        ++state->guard_reads;
        state->guard_bytes.fetch_add(static_cast<uint32_t>(release.size()));
        EXPECT_TRUE(win.memory.try_read_memory(pool_fixture::base + 0xB440BF, release.data(), release.size()));
        constexpr std::array<uint8_t, 7> expected_poll{0x41, 0x8B, 0x97, 0x68, 0x00, 0x22, 0x00};
        constexpr std::array<uint8_t, 5> expected_release{0xE8, 0x4C, 0x12, 0x8E, 0xFF};
        EXPECT_EQ(poll, expected_poll);
        EXPECT_EQ(release, expected_release);
    }

    void record_child_fixture(const std::shared_ptr<child_reporter_state>& state, const std::string& prefix)
    {
        const auto records = snapshot_child_records(state);
        size_t sum{}, maximum{};
        uint64_t rejection_max{};
        std::map<uint32_t, std::pair<size_t, size_t>> owners;
        std::map<uint32_t, child_callback_counts> callbacks;
        {
            const std::lock_guard guard(state->mutex);
            callbacks = state->callback_counts;
        }
        std::string sizes;
        for (const auto& record : records)
        {
            sum += record.serialized_bytes;
            maximum = std::max(maximum, record.serialized_bytes);
            rejection_max = std::max(rejection_max, record.sample->output_rejections);
            ++owners[record.attempt].first;
            owners[record.attempt].second += record.serialized_bytes;
            sizes += std::to_string(record.attempt) + ":" + std::to_string(static_cast<unsigned>(record.sample->kind)) + ":" +
                     std::to_string(record.sample->site) + ":" + std::to_string(record.serialized_bytes) + ";";
        }
        uint64_t callback_attempts{}, callback_charges{}, callback_exceptions{}, callback_zero_returns{}, callback_bytes{};
        for (const auto& [attempt, callback] : callbacks)
        {
            const auto counts = owners[attempt];
            EXPECT_LE(callback.attempts, 7U);
            EXPECT_EQ(callback.attempts, callback.charges + callback.exceptions + callback.zero_returns);
            EXPECT_LE(counts.first, 7U);
            EXPECT_LE(counts.second, native_child_limits::lifecycle_bytes);
            EXPECT_LE(callback.charged_bytes, counts.second);
            callback_attempts += callback.attempts;
            callback_charges += callback.charges;
            callback_exceptions += callback.exceptions;
            callback_zero_returns += callback.zero_returns;
            callback_bytes += callback.charged_bytes;
            testing::Test::RecordProperty(prefix + "_observer_attempt_" + std::to_string(attempt) + "_observed_records",
                                          std::to_string(counts.first));
            testing::Test::RecordProperty(prefix + "_observer_attempt_" + std::to_string(attempt) + "_observed_bytes",
                                          std::to_string(counts.second));
            const auto key = prefix + "_observer_attempt_" + std::to_string(attempt);
            testing::Test::RecordProperty(key + "_callback_attempts", std::to_string(callback.attempts));
            testing::Test::RecordProperty(key + "_callback_positive_charges", std::to_string(callback.charges));
            testing::Test::RecordProperty(key + "_callback_charged_bytes", std::to_string(callback.charged_bytes));
            testing::Test::RecordProperty(key + "_callback_exceptions", std::to_string(callback.exceptions));
            testing::Test::RecordProperty(key + "_callback_zero_returns", std::to_string(callback.zero_returns));
        }
        EXPECT_EQ(callback_attempts, state->callback_attempts.load());
        EXPECT_EQ(callback_charges, state->callback_charges.load());
        EXPECT_EQ(callback_exceptions, state->callback_exceptions.load());
        EXPECT_EQ(callback_zero_returns, state->callback_zero_returns.load());
        EXPECT_EQ(callback_bytes, state->charged_bytes.load());
        EXPECT_LE(state->charged_bytes.load(), sum);
        if (!state->child_throws.load())
        {
            EXPECT_EQ(state->charged_bytes.load(), sum);
            EXPECT_EQ(state->callback_charges.load(), records.size());
        }
        testing::Test::RecordProperty(prefix + "_records_attempt_kind_site_bytes", sizes);
        testing::Test::RecordProperty(prefix + "_observed_memory_records", std::to_string(records.size()));
        testing::Test::RecordProperty(prefix + "_observed_serialized_sum", std::to_string(sum));
        testing::Test::RecordProperty(prefix + "_maximum_record_bytes", std::to_string(maximum));
        testing::Test::RecordProperty(prefix + "_callback_attempts", std::to_string(state->callback_attempts.load()));
        testing::Test::RecordProperty(prefix + "_callback_positive_charges", std::to_string(state->callback_charges.load()));
        testing::Test::RecordProperty(prefix + "_callback_zero_returns", std::to_string(state->callback_zero_returns.load()));
        testing::Test::RecordProperty(prefix + "_callback_charged_bytes", std::to_string(state->charged_bytes.load()));
        testing::Test::RecordProperty(prefix + "_child_throw_attempts", std::to_string(state->child_throws.load()));
        testing::Test::RecordProperty(prefix + "_flush_throw_attempts", std::to_string(state->flush_throws.load()));
        testing::Test::RecordProperty(prefix + "_callback_exceptions", std::to_string(state->callback_exceptions.load()));
        testing::Test::RecordProperty(prefix + "_output_rejection_witness_max", std::to_string(rejection_max));
        testing::Test::RecordProperty(prefix + "_worker_start_attempts", std::to_string(state->worker_attempts.load()));
        testing::Test::RecordProperty(prefix + "_worker_start_entries", std::to_string(state->worker_entries.load()));
        testing::Test::RecordProperty(prefix + "_hook_install_attempts", std::to_string(state->hook_attempts.load()));
        testing::Test::RecordProperty(prefix + "_hook_install_api_returns", std::to_string(state->actual_installs.load()));
        testing::Test::RecordProperty(prefix + "_injected_failure_count", std::to_string(state->injected_failures.load()));
        testing::Test::RecordProperty(prefix + "_kernel_failed_try_lock_witnesses", std::to_string(state->kernel_waits.load()));
        testing::Test::RecordProperty(prefix + "_worker_stop_requests", std::to_string(state->worker_stop_requests.load()));
        testing::Test::RecordProperty(prefix + "_worker_exits", std::to_string(state->worker_exits.load()));
        testing::Test::RecordProperty(prefix + "_external_joins", std::to_string(state->external_joins.load()));
        testing::Test::RecordProperty(prefix + "_overlap_events", std::to_string(state->overlaps.load()));
        testing::Test::RecordProperty(prefix + "_intentional_bypass_control_events", std::to_string(state->intentional_overlaps.load()));
        testing::Test::RecordProperty(prefix + "_reporter_gate_timeouts", std::to_string(state->gate_timeouts.load()));
        testing::Test::RecordProperty(prefix + "_watchdog_timeout_exit", "86");
        testing::Test::RecordProperty(prefix + "_watchdog_seconds", "20");
        testing::Test::RecordProperty(prefix + "_original_byte_read_attempts", std::to_string(state->guard_reads.load()));
        testing::Test::RecordProperty(prefix + "_original_byte_requested_bytes", std::to_string(state->guard_bytes.load()));
        testing::Test::RecordProperty(prefix + "_persistent_writer_retention_available", "false");
        testing::Test::RecordProperty(prefix + "_per_vm_physical_removal_available", "false");
        testing::Test::RecordProperty(prefix + "_live_removal_cost_available", "false");
        testing::Test::RecordProperty(prefix + "_native_return_boundary_executed", "false");
        EXPECT_EQ(state->gate_timeouts.load(), 0U);
        EXPECT_EQ(state->after_destroy.load(), 0U);
    }
}

TEST(NativeChildLifecycle, PendingNormalAndFailureFinishBeforeFooterFlushAndOwnerDestruction)
{
    child_lifecycle_environment environment;
    for (const bool failed : {false, true})
    {
        auto state = std::make_shared<child_reporter_state>();
        child_fixture_watchdog watchdog(state);
        sogen::analysis_settings settings{};
        sogen::analysis_context context{.settings = &settings};
        auto win = child_lifecycle_emulator();
        context.win_emu = win.get();
        {
            std::vector<std::unique_ptr<sogen::analysis_reporter>> reporters;
            reporters.push_back(std::make_unique<child_lifecycle_reporter>(state));
            context.reporters = {reporters[0].get()};
            sogen::native_child_capture_scope capture{*win};
            ASSERT_TRUE(arm_child_lifecycle(*win, context, state));
            state->phase.store("terminal_finish");
            ASSERT_TRUE(capture.finish());
            const auto before = snapshot_child_records(state);
            constexpr std::array expected{native_child_kind::selection, native_child_kind::activation, native_child_kind::coverage,
                                          native_child_kind::coverage,  native_child_kind::removed,    native_child_kind::removed,
                                          native_child_kind::coverage};
            ASSERT_EQ(before.size(), expected.size());
            for (size_t index = 0; index < expected.size(); ++index)
            {
                EXPECT_EQ(before[index].sample->kind, expected[index]);
            }
            for (const auto& site : before.back().sample->sites)
            {
                EXPECT_NE(site.installed_ns, 0U);
                EXPECT_NE(site.retired_ns, 0U);
                EXPECT_NE(site.removed_ns, 0U);
            }
            EXPECT_EQ(state->worker_entries.load(), 1U);
            EXPECT_EQ(state->worker_exits.load(), 1U);
            EXPECT_FALSE(static_cast<bool>(win->callbacks.on_native_child_capture));
            check_child_original_bytes(*win, state);
            if (failed)
            {
                context.emit_summary<sogen::run_failed_event>([](auto& event) { event.message = "fixture failure"; });
            }
            else
            {
                context.emit_summary<sogen::run_finished_event>([](auto& event) { event.success = true; });
            }
            flush_child_lifecycle(*win, context);
            EXPECT_TRUE(capture.finish());
            EXPECT_TRUE(win->shutdown_native_child_capture());
            EXPECT_EQ(snapshot_child_records(state).size(), before.size());
            {
                const std::lock_guard guard(state->mutex);
                ASSERT_GE(state->order.size(), 2U);
                EXPECT_EQ(state->order[state->order.size() - 2], failed ? "failed" : "finished");
                EXPECT_EQ(state->order.back(), "flush");
            }
        }
        const auto count = snapshot_child_records(state).size();
        win.reset();
        EXPECT_TRUE(state->destroyed.load());
        EXPECT_EQ(snapshot_child_records(state).size(), count);
        EXPECT_EQ(state->overlaps.load(), 0U);
        record_child_fixture(state, failed ? "failure" : "normal");
    }
}

TEST(NativeChildLifecycle, OuterScopeStopsPendingCaptureOnExceptionUnwind)
{
    child_lifecycle_environment environment;
    for (const auto failure : {child_failure::worker_start, child_failure::first_install, child_failure::second_install,
                               child_failure::guard, child_failure::selection_report, child_failure::none})
    {
        auto state = std::make_shared<child_reporter_state>();
        child_fixture_watchdog watchdog(state);
        sogen::analysis_settings settings{};
        sogen::analysis_context context{.settings = &settings};
        auto win = child_lifecycle_emulator();
        context.win_emu = win.get();
        if (failure == child_failure::none)
        {
            EXPECT_THROW(
                [&] {
                    std::vector<std::unique_ptr<sogen::analysis_reporter>> reporters;
                    reporters.push_back(std::make_unique<child_lifecycle_reporter>(state));
                    context.reporters = {reporters[0].get()};
                    sogen::native_child_capture_scope capture{*win};
                    const auto armed = arm_child_lifecycle(*win, context, state);
                    if (!armed)
                    {
                        ADD_FAILURE() << armed.message();
                        throw std::runtime_error("fixture_arm_failure");
                    }
                    state->phase.store("outer_unwind");
                    throw std::runtime_error("outer failure");
                }(),
                std::runtime_error);
            EXPECT_TRUE(state->destroyed.load());
            EXPECT_EQ(state->worker_entries.load(), state->worker_exits.load());
            const auto records = snapshot_child_records(state);
            ASSERT_FALSE(records.empty());
            for (const auto& site : records.back().sample->sites)
            {
                EXPECT_NE(site.installed_ns, 0U);
                EXPECT_NE(site.removed_ns, 0U);
            }
            const auto count = records.size();
            win.reset();
            EXPECT_EQ(snapshot_child_records(state).size(), count);
            record_child_fixture(state, "outer_unwind");
            continue;
        }
        {
            std::vector<std::unique_ptr<sogen::analysis_reporter>> reporters;
            reporters.push_back(std::make_unique<child_lifecycle_reporter>(state));
            context.reporters = {reporters[0].get()};
            sogen::native_child_capture_scope capture{*win};
            if (failure == child_failure::guard)
            {
                const uint8_t mismatched = 0x90;
                win->memory.write_memory(pool_fixture::base + 0x43C5D9, &mismatched, sizeof(mismatched));
            }
            if (failure == child_failure::selection_report)
            {
                state->throw_child.store(true);
            }
            const auto armed = arm_child_lifecycle(*win, context, state, failure);
            EXPECT_FALSE(armed);
            {
                if (failure == child_failure::worker_start)
                {
                    EXPECT_EQ(state->injected_failures.load(), 1U);
                    EXPECT_EQ(state->actual_installs.load(), 0U);
                    EXPECT_FALSE(sogen::native_child_capture_test_access::inspect(*win).present);
                    ASSERT_TRUE(arm_child_lifecycle(*win, context, state));
                    EXPECT_EQ(state->worker_attempts.load(), 2U);
                }
                if (failure == child_failure::first_install || failure == child_failure::second_install)
                {
                    EXPECT_EQ(state->injected_failures.load(), 1U);
                    EXPECT_EQ(state->hook_attempts.load(), 2U);
                    EXPECT_EQ(state->actual_installs.load(), 1U);
                    EXPECT_EQ(sogen::native_child_capture_test_access::inspect(*win).owned_hooks, 1U);
                }
                if (failure == child_failure::guard)
                {
                    EXPECT_EQ(state->actual_installs.load(), 0U);
                    const auto rejected = snapshot_child_records(state);
                    const auto activation = std::find_if(rejected.begin(), rejected.end(), [](const auto& record) {
                        return record.sample->kind == native_child_kind::activation;
                    });
                    ASSERT_NE(activation, rejected.end());
                    EXPECT_EQ(activation->sample->reason, native_child_reason::guard_mismatch);
                }
                if (failure == child_failure::selection_report)
                {
                    EXPECT_GT(state->child_throws.load(), 0U);
                    EXPECT_GT(state->callback_exceptions.load(), 0U);
                    EXPECT_EQ(state->worker_attempts.load(), 0U);
                    EXPECT_EQ(state->actual_installs.load(), 0U);
                }
            }
            ASSERT_TRUE(capture.finish());
            EXPECT_FALSE(static_cast<bool>(win->callbacks.on_native_child_capture));
            EXPECT_FALSE(sogen::native_child_capture_test_access::inspect(*win).present);
            EXPECT_EQ(state->worker_entries.load(), state->worker_exits.load());
            const auto records = snapshot_child_records(state);
            ASSERT_FALSE(records.empty());
            for (const auto& site : records.back().sample->sites)
            {
                if (site.installed_ns)
                {
                    EXPECT_NE(site.removed_ns, 0U);
                }
            }
            if (failure == child_failure::first_install || failure == child_failure::second_install)
            {
                const auto count = records.size();
                EXPECT_FALSE(arm_child_lifecycle(*win, context, state));
                EXPECT_EQ(snapshot_child_records(state).size(), count);
            }
        }
        const auto count = snapshot_child_records(state).size();
        win.reset();
        EXPECT_EQ(snapshot_child_records(state).size(), count);
        record_child_fixture(state, "setup_" + std::to_string(static_cast<unsigned>(failure)));
        if (failure == child_failure::first_install || failure == child_failure::second_install)
        {
            state->phase.store("fresh_owner_retry");
            auto retry_state = std::make_shared<child_reporter_state>();
            sogen::analysis_context retry_context{.settings = &settings};
            auto retry_win = child_lifecycle_emulator();
            retry_context.win_emu = retry_win.get();
            std::vector<std::unique_ptr<sogen::analysis_reporter>> retry_reporters;
            retry_reporters.push_back(std::make_unique<child_lifecycle_reporter>(retry_state));
            retry_context.reporters = {retry_reporters[0].get()};
            sogen::native_child_capture_scope retry_capture{*retry_win};
            ASSERT_TRUE(arm_child_lifecycle(*retry_win, retry_context, retry_state));
            ASSERT_TRUE(retry_capture.finish());
            EXPECT_EQ(retry_state->actual_installs.load(), 2U);
            EXPECT_EQ(retry_state->worker_entries.load(), retry_state->worker_exits.load());
            record_child_fixture(retry_state, "fresh_retry_" + std::to_string(static_cast<unsigned>(failure)));
        }
    }
}

TEST(NativeChildLifecycle, ReporterAndFlushExceptionsDoNotEscapeChildShutdown)
{
    child_lifecycle_environment environment;
    auto state = std::make_shared<child_reporter_state>();
    child_fixture_watchdog watchdog(state);
    sogen::analysis_settings settings{};
    sogen::analysis_context context{.settings = &settings};
    auto win = child_lifecycle_emulator();
    context.win_emu = win.get();
    {
        std::vector<std::unique_ptr<sogen::analysis_reporter>> reporters;
        reporters.push_back(std::make_unique<child_lifecycle_reporter>(state));
        context.reporters = {reporters[0].get()};
        sogen::native_child_capture_scope capture{*win};
        ASSERT_TRUE(arm_child_lifecycle(*win, context, state));
        state->throw_child.store(true);
        EXPECT_TRUE(capture.finish());
        EXPECT_GT(state->child_throws.load(), 0U);
        EXPECT_EQ(state->callback_exceptions.load(), state->child_throws.load());
        const auto records = snapshot_child_records(state);
        ASSERT_FALSE(records.empty());
        EXPECT_GT(records.back().sample->output_rejections, 0U);
        state->throw_flush.store(true);
        try
        {
            flush_child_lifecycle(*win, context);
            FAIL() << "Expected actual flush reporter exception";
        }
        catch (const std::runtime_error& error)
        {
            EXPECT_STREQ(error.what(), "fixture_flush_throw");
        }
        EXPECT_EQ(state->flush_throws.load(), 1U);
        EXPECT_TRUE(capture.finish());
        EXPECT_FALSE(static_cast<bool>(win->callbacks.on_native_child_capture));
    }
    win.reset();
    EXPECT_EQ(state->overlaps.load(), 0U);
    record_child_fixture(state, "throw");
}

TEST(NativeChildLifecycle, ResumablePauseFlushRetainsSelectedCaptureUntilTerminalFinish)
{
    child_lifecycle_environment environment;
    auto state = std::make_shared<child_reporter_state>();
    child_fixture_watchdog watchdog(state);
    sogen::analysis_settings settings{};
    sogen::analysis_context context{.settings = &settings};
    auto win = child_lifecycle_emulator();
    context.win_emu = win.get();
    std::vector<std::unique_ptr<sogen::analysis_reporter>> reporters;
    reporters.push_back(std::make_unique<child_lifecycle_reporter>(state));
    context.reporters = {reporters[0].get()};
    sogen::native_child_capture_scope capture{*win};
    ASSERT_TRUE(arm_child_lifecycle(*win, context, state));
    const auto before = sogen::native_child_capture_test_access::inspect(*win);
    uint32_t writer_entries{};
    std::vector<std::byte> snapshot;
    for (size_t pause = 0; pause < 2; ++pause)
    {
        EXPECT_TRUE(sogen::snapshot::is_resumable_checkpoint_stop(win->last_stop_reason()));
        EXPECT_THROW(win->synchronize_native_child_capture(
                         [&] {
                             ++writer_entries;
                             snapshot = sogen::snapshot::create_emulator_snapshot(*win);
                         },
                         true),
                     std::runtime_error);
        flush_child_lifecycle(*win, context);
    }
    EXPECT_EQ(writer_entries, 0U);
    EXPECT_TRUE(snapshot.empty());
    const auto retained = sogen::native_child_capture_test_access::inspect(*win);
    EXPECT_EQ(retained.selection.selected_ns, before.selection.selected_ns);
    EXPECT_EQ(retained.selection.marker_sequence, before.selection.marker_sequence);
    EXPECT_EQ(retained.owned_hooks, before.owned_hooks);
    EXPECT_EQ(retained.collection_ns, before.collection_ns);
    EXPECT_EQ(retained.callback_ns, before.callback_ns);
    for (size_t index = 0; index < 2; ++index)
    {
        EXPECT_EQ(retained.sites[index].installed_ns, before.sites[index].installed_ns);
        EXPECT_EQ(retained.sites[index].attempts, before.sites[index].attempts);
        EXPECT_EQ(retained.sites[index].active_traps, before.sites[index].active_traps);
        EXPECT_EQ(retained.sites[index].retired_ns, 0U);
    }
    ASSERT_TRUE(capture.finish());
    state->phase.store("actual_snapshot_roundtrip");
    constexpr uint64_t sentinel_address = pool_fixture::base + 0x700000;
    constexpr uint64_t source_sentinel = 0x13579BDF2468ACE0;
    constexpr uint64_t destination_sentinel = 0xFEDCBA9876543210;
    ASSERT_TRUE(win->memory.allocate_memory(sentinel_address, 0x1000, sogen::memory_permission::read_write));
    win->memory.write_memory(sentinel_address, &source_sentinel, sizeof(source_sentinel));
    ASSERT_NO_THROW(win->synchronize_native_child_capture(
        [&] {
            ++writer_entries;
            snapshot = sogen::snapshot::create_emulator_snapshot(*win);
        },
        true));
    ASSERT_GT(snapshot.size(), 8U);
    EXPECT_EQ(writer_entries, 1U);
    auto restored = child_lifecycle_emulator();
    ASSERT_TRUE(restored->memory.allocate_memory(sentinel_address, 0x1000, sogen::memory_permission::read_write));
    restored->memory.write_memory(sentinel_address, &destination_sentinel, sizeof(destination_sentinel));
    uint64_t before_load{};
    ASSERT_TRUE(restored->memory.try_read_memory(sentinel_address, &before_load, sizeof(before_load)));
    ASSERT_EQ(before_load, destination_sentinel);
    ASSERT_NE(before_load, source_sentinel);
    ASSERT_NO_THROW(sogen::snapshot::load_emulator_snapshot(*restored, std::span<const std::byte>{snapshot}));
    uint64_t after_load{};
    ASSERT_TRUE(restored->memory.try_read_memory(sentinel_address, &after_load, sizeof(after_load)));
    EXPECT_EQ(after_load, source_sentinel);
    EXPECT_EQ(restored->get_executed_instructions(), win->get_executed_instructions());
    EXPECT_FALSE(sogen::native_child_capture_test_access::inspect(*restored).present);
    check_child_original_bytes(*restored, state);
    testing::Test::RecordProperty("resume_actual_snapshot_bytes", std::to_string(snapshot.size()));
    testing::Test::RecordProperty("resume_actual_snapshot_roundtrip", "true");
    testing::Test::RecordProperty("resume_sentinel_before_load", std::to_string(before_load));
    testing::Test::RecordProperty("resume_sentinel_after_load", std::to_string(after_load));
    testing::Test::RecordProperty("resume_sentinel_expected_source", std::to_string(source_sentinel));
    testing::Test::RecordProperty("resume_analyzer_checkpoint_loop_executed", "false");
    testing::Test::RecordProperty("resume_guest_execution", "false");
    EXPECT_EQ(state->overlaps.load(), 0U);
    record_child_fixture(state, "resume");
}

TEST(NativeChildLifecycle, IntermediateFlushWaitsForWorkerReportWithoutClosingReporterCallback)
{
    child_lifecycle_environment environment;
    for (const bool stop : {false, true})
    {
        auto state = std::make_shared<child_reporter_state>();
        child_fixture_watchdog watchdog(state);
        sogen::analysis_settings settings{};
        sogen::analysis_context context{.settings = &settings};
        auto win = child_lifecycle_emulator();
        context.win_emu = win.get();
        std::vector<std::unique_ptr<sogen::analysis_reporter>> reporters;
        reporters.push_back(std::make_unique<child_lifecycle_reporter>(state));
        context.reporters = {reporters[0].get()};
        sogen::native_child_capture_scope capture{*win};
        ASSERT_TRUE(arm_child_lifecycle(*win, context, state));
        const auto release = sogen::utils::finally([&] { release_child_reporter(state); });
        state->block_coverage.store(true);
        state->phase.store("await_original_poll_lease");
        {
            std::unique_lock guard(state->mutex);
            ASSERT_TRUE(state->condition.wait_for(guard, std::chrono::seconds(5), [&] { return state->coverage_entered; }));
        }
        EXPECT_EQ(state->inside.load(), 1U);
        reporters[0]->flush();
        ++state->intentional_overlaps;
        EXPECT_EQ(state->overlaps.load(), 1U);
        std::atomic<bool> shutdown_result{};
        const auto on_wait = [state] {
            {
                const std::lock_guard guard(state->mutex);
                ++state->kernel_waits;
                state->phase.store("external_kernel_wait");
            }
            state->condition.notify_all();
        };
        child_external_operation operation(state, [&] {
            if (stop)
            {
                shutdown_result.store(sogen::native_child_capture_test_access::shutdown(*win, on_wait));
            }
            else
            {
                sogen::native_child_capture_test_access::flush(*win, [&] { reporters[0]->flush(); }, on_wait);
            }
        });
        {
            std::unique_lock guard(state->mutex);
            ASSERT_TRUE(state->condition.wait_for(guard, std::chrono::seconds(5), [&] { return state->kernel_waits.load() == 1; }));
        }
        EXPECT_EQ(state->external_entries.load(), 1U);
        EXPECT_FALSE(operation.completed(std::chrono::milliseconds(50)));
        EXPECT_EQ(state->inside.load(), 1U);
        EXPECT_EQ(state->worker_stop_requests.load(), 0U);
        release_child_reporter(state);
        operation.join();
        EXPECT_NO_THROW(operation.rethrow());
        EXPECT_EQ(state->external_joins.load(), 1U);
        EXPECT_EQ(state->overlaps.load(), state->intentional_overlaps.load());
        if (stop)
        {
            EXPECT_TRUE(shutdown_result.load());
            EXPECT_EQ(state->worker_stop_requests.load(), 1U);
            EXPECT_EQ(state->worker_exits.load(), 1U);
            EXPECT_FALSE(static_cast<bool>(win->callbacks.on_native_child_capture));
        }
        else
        {
            EXPECT_TRUE(static_cast<bool>(win->callbacks.on_native_child_capture));
            EXPECT_EQ(state->flushes.load(), 2U);
        }
        ASSERT_TRUE(capture.finish());
        record_child_fixture(state, stop ? "blocked_stop" : "blocked_flush");
    }
}

TEST(NativeChildLifecycle, NonquiescentRemovalIsUnqualifiedAndEmissionClosesBeforeReporterTeardown)
{
    child_lifecycle_environment environment;
    auto state = std::make_shared<child_reporter_state>();
    child_fixture_watchdog watchdog(state);
    sogen::analysis_settings settings{};
    sogen::analysis_context context{.settings = &settings};
    auto win = child_lifecycle_emulator();
    context.win_emu = win.get();
    {
        std::vector<std::unique_ptr<sogen::analysis_reporter>> reporters;
        reporters.push_back(std::make_unique<child_lifecycle_reporter>(state));
        context.reporters = {reporters[0].get()};
        sogen::native_child_capture_scope capture{*win};
        ASSERT_TRUE(arm_child_lifecycle(*win, context, state));
        const auto reset_running = sogen::utils::finally([&] { win->vcpu(0).running.store(false); });
        win->vcpu(0).running.store(true);
        EXPECT_FALSE(capture.finish());
        EXPECT_FALSE(capture.finish());
        EXPECT_FALSE(static_cast<bool>(win->callbacks.on_native_child_capture));
        const auto pending = sogen::native_child_capture_test_access::inspect(*win);
        EXPECT_TRUE(pending.present);
        EXPECT_TRUE(pending.reporting_stopped);
        EXPECT_EQ(pending.owned_hooks, 2U);
        for (const auto& site : pending.sites)
        {
            EXPECT_NE(site.installed_ns, 0U);
            EXPECT_EQ(site.removed_ns, 0U);
        }
    }
    EXPECT_FALSE(win->vcpu(0).running.load());
    const auto before_records = snapshot_child_records(state).size();
    const auto before_attempts = state->callback_attempts.load();
    const auto before_charged = state->charged_bytes.load();
    ASSERT_TRUE(win->shutdown_native_child_capture());
    check_child_original_bytes(*win, state);
    win.reset();
    EXPECT_EQ(snapshot_child_records(state).size(), before_records);
    EXPECT_EQ(state->callback_attempts.load(), before_attempts);
    EXPECT_EQ(state->charged_bytes.load(), before_charged);
    EXPECT_EQ(state->after_destroy.load(), 0U);
    testing::Test::RecordProperty("pending_real_vcpu_execution", "false");
    record_child_fixture(state, "pending");
}

TEST(OptionalFunction, MutableAndConstCopiesPreserveIndependentCallableState)
{
    using function = sogen::utils::optional_function<int()>;
    function original([value = 3]() mutable { return ++value; });
    function mutable_copy(original);
    const function immutable(original);
    function const_copy(immutable);

    EXPECT_EQ(mutable_copy(), 4);
    EXPECT_EQ(original(), 4);
    EXPECT_EQ(const_copy(), 4);
    EXPECT_EQ(immutable(), 4);
    EXPECT_EQ(mutable_copy(), 5);
}

TEST(OptionalFunction, CopyAssignmentAndEmptyWrappersPreserveValueSemantics)
{
    using function = sogen::utils::optional_function<int()>;
    function original([value = 10]() mutable { return ++value; });
    function assigned([] { return -1; });
    assigned = original;
    const function immutable(original);
    function const_assigned;
    const_assigned = immutable;
    EXPECT_EQ(assigned(), 11);
    EXPECT_EQ(original(), 11);
    EXPECT_EQ(const_assigned(), 11);

    auto& same = assigned;
    assigned = same;
    EXPECT_EQ(assigned(), 12);
    function empty;
    function empty_copy(empty);
    assigned = empty;
    EXPECT_FALSE(empty_copy);
    EXPECT_FALSE(assigned);
    EXPECT_EQ(assigned(), 0);
    const function const_empty;
    assigned = const_empty;
    EXPECT_FALSE(assigned);
}

TEST(OptionalFunction, MovesAndOrdinaryCallablesRetainTheirExistingContracts)
{
    using function = sogen::utils::optional_function<int(int)>;
    static_assert(std::is_nothrow_move_constructible_v<function>);
    static_assert(std::is_nothrow_move_assignable_v<function>);
    function source([](int value) { return value + 4; });
    function moved(std::move(source));
    EXPECT_EQ(moved(8), 12);
    function assigned;
    assigned = std::move(moved);
    EXPECT_EQ(assigned(9), 13);

    std::function<int(int)> typed = [](int value) { return value * 2; };
    function converted(typed);
    EXPECT_EQ(converted(6), 12);
    converted = [](int value) { return value - 1; };
    EXPECT_EQ(converted(6), 5);
    sogen::utils::optional_function<void()> empty;
    EXPECT_NO_THROW(empty());
    int calls{};
    sogen::utils::optional_function<void()> callback([&] { ++calls; });
    auto copied = callback;
    copied();
    EXPECT_EQ(calls, 1);
}

TEST(OptionalFunction, NativeChildCallbackCopyAndCapturePreservePayloadAndByteResult)
{
    using function = sogen::utils::optional_function<size_t(std::shared_ptr<const native_child_sample>, size_t)>;
    uint32_t calls{};
    function original([&](std::shared_ptr<const native_child_sample> sample, const size_t maximum_bytes) {
        ++calls;
        return sample ? maximum_bytes : size_t{0};
    });
    const auto saved = original;
    function assigned;
    assigned = original;
    function wrapped([saved](std::shared_ptr<const native_child_sample> sample, const size_t maximum_bytes) {
        return saved(std::move(sample), maximum_bytes);
    });
    const auto payload = std::make_shared<const native_child_sample>();
    EXPECT_EQ(saved(payload, 32), size_t{32});
    EXPECT_EQ(assigned(payload, 48), size_t{48});
    EXPECT_EQ(wrapped(payload, 64), size_t{64});
    EXPECT_EQ(calls, 3U);
    function empty;
    const auto empty_copy = empty;
    assigned = empty;
    EXPECT_FALSE(empty_copy);
    EXPECT_FALSE(assigned);
    EXPECT_EQ(assigned(payload, 64), size_t{0});
    EXPECT_EQ(calls, 3U);
}
