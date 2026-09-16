#include <native_wsi_wire.hpp>
#include <gtest/gtest.h>
#include <limits>

namespace sogen::test
{
    namespace wire = gpu_bridge::native_wsi;

    TEST(NativeWsiWire, PointerWidthIndependentIdsAndTimeoutRemainExact)
    {
        wire::request input{};
        input.op = wire::operation::acquire;
        input.owner = 0xF123456789ABCDEF;
        input.object = 0x80000000FEDCBA98;
        input.timeout = UINT64_MAX;
        const auto bytes = wire::encode(input);
        ASSERT_EQ(bytes.size(), 120u);
        EXPECT_EQ(bytes[16], std::byte{0xEF});
        EXPECT_EQ(bytes[23], std::byte{0xF1});
        EXPECT_EQ(bytes[40], std::byte{0xFF});
        wire::request decoded{};
        ASSERT_TRUE(wire::decode(bytes, decoded));
        EXPECT_EQ(decoded.owner, input.owner);
        EXPECT_EQ(decoded.object, input.object);
        EXPECT_EQ(decoded.timeout, UINT64_MAX);
    }

    TEST(NativeWsiWire, SwapchainFieldsAndBothArraysRoundTrip)
    {
        wire::request input{};
        input.op = wire::operation::create_swapchain;
        for (size_t i = 0; i < wire::family_count; ++i)
        {
            input.words[i] = static_cast<uint32_t>(31 + i);
        }
        input.words[wire::family_count] = 2;
        input.words[wire::format_count] = 3;
        const std::array<uint32_t, 5> data{1, 7, 37, 43, 50};
        auto bytes = wire::encode(input, std::as_bytes(std::span{data}));
        wire::request decoded{};
        ASSERT_TRUE(wire::decode(bytes, decoded));
        EXPECT_EQ(decoded.words, input.words);
        EXPECT_EQ(std::memcmp(bytes.data() + sizeof(input), data.data(), sizeof(data)), 0);
        bytes.pop_back();
        EXPECT_FALSE(wire::decode(bytes, decoded));
    }

    TEST(NativeWsiWire, PresentWaitsBelongToOneBatchWithAllSwapchains)
    {
        wire::request input{};
        input.op = wire::operation::present;
        input.words[0] = 2;
        input.words[1] = 2;
        const std::array<uint64_t, 2> waits{0xF000000000000001, 0xF000000000000002};
        const std::array<wire::present_entry, 2> entries{{{.swapchain = 0xA000000000000001, .image_index = 3, .reserved = 0},
                                                          {.swapchain = 0xB000000000000001, .image_index = 1, .reserved = 0}}};
        std::vector<std::byte> trailing(sizeof(waits) + sizeof(entries));
        std::memcpy(trailing.data(), waits.data(), sizeof(waits));
        std::memcpy(trailing.data() + sizeof(waits), entries.data(), sizeof(entries));
        auto bytes = wire::encode(input, trailing);
        wire::request decoded{};
        ASSERT_TRUE(wire::decode(bytes, decoded));
        EXPECT_EQ(decoded.words[0], 2u);
        EXPECT_EQ(decoded.words[1], 2u);
        EXPECT_EQ(bytes.size(), 168u);
        bytes.back() = std::byte{1};
        EXPECT_FALSE(wire::decode(bytes, decoded));
    }

    TEST(NativeWsiWire, CountOnlyAndNonNullZeroCapacityRemainDistinct)
    {
        wire::request count{};
        count.op = wire::operation::query_surface;
        count.words[0] = static_cast<uint32_t>(wire::query::formats);
        auto capacity_zero = count;
        capacity_zero.words[3] = 1;
        const auto a = wire::encode(count);
        const auto b = wire::encode(capacity_zero);
        EXPECT_NE(a, b);
        wire::request decoded{};
        EXPECT_TRUE(wire::decode(a, decoded));
        EXPECT_EQ(decoded.words[3], 0u);
        EXPECT_TRUE(wire::decode(b, decoded));
        EXPECT_EQ(decoded.words[2], 0u);
        EXPECT_EQ(decoded.words[3], 1u);
    }

    TEST(NativeWsiWire, RejectsInvalidVersionReservedOperationAndSize)
    {
        wire::request input{};
        wire::request decoded{};
        input.protocol = 2;
        EXPECT_FALSE(wire::decode(wire::encode(input), decoded));
        input.protocol = wire::version;
        input.reserved = 1;
        EXPECT_FALSE(wire::decode(wire::encode(input), decoded));
        input.reserved = 0;
        input.op = static_cast<wire::operation>(UINT32_MAX);
        EXPECT_FALSE(wire::decode(wire::encode(input), decoded));
        input.op = wire::operation::enabled;
        auto bytes = wire::encode(input);
        bytes[8] = std::byte{0};
        EXPECT_FALSE(wire::decode(bytes, decoded));
        EXPECT_FALSE(wire::decode(std::span<const std::byte>{}, decoded));
    }

    TEST(NativeWsiWire, RejectsArrayCountOverflowAndTrailingGarbage)
    {
        wire::request input{};
        wire::request decoded{};
        input.op = wire::operation::present;
        input.words[0] = UINT32_MAX;
        EXPECT_FALSE(wire::decode(wire::encode(input), decoded));
        input.op = wire::operation::create_swapchain;
        input.words[wire::family_count] = UINT32_MAX;
        EXPECT_FALSE(wire::decode(wire::encode(input), decoded));
        input = {};
        const std::array<std::byte, 1> extra{std::byte{1}};
        EXPECT_FALSE(wire::decode(wire::encode(input, extra), decoded));
        size_t size = wire::max_packet - 4;
        EXPECT_TRUE(wire::append_size(size, 1, 4));
        EXPECT_FALSE(wire::append_size(size, 1, 4));
        EXPECT_EQ(size, wire::max_packet);
    }
}
