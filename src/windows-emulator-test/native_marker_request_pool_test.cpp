#include <native_marker_request_pool.hpp>
#include <native_marker_capture_state.hpp>
#include <algorithm>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <tuple>
#include <vector>

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
