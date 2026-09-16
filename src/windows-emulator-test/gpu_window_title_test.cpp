#include <ui_backends/gpu_window_title.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace sogen::test
{
    namespace
    {
        constexpr std::string_view identity = "Type: discrete | Adapter: AMD Radeon RX 6700";
        constexpr std::string_view metrics = "Util: 75% | VRAM: 1024/10240 MiB | Mem: 2048 MiB";

        std::string encode_record(const std::string_view pid = "1234", const std::string_view timestamp = "10000",
                                  const std::string_view hwnd = "4660", const std::string_view identity_line = identity,
                                  const std::string_view metrics_line = metrics)
        {
            std::string bytes{"SOGEN_GPU_TITLE_V1 "};
            bytes += pid;
            bytes += ' ';
            bytes += timestamp;
            bytes += ' ';
            bytes += hwnd;
            bytes += '\n';
            bytes += identity_line;
            bytes += '\n';
            bytes += metrics_line;
            bytes += '\n';
            return bytes;
        }
    }

    TEST(GpuWindowTitleTest, DecodesOwnedIdentityAndMetrics)
    {
        auto bytes = encode_record();
        const auto record = ui::decode_gpu_window_title_record(bytes);
        ASSERT_TRUE(record.has_value());
        bytes.assign(bytes.size(), 'X');
        EXPECT_EQ(record->host_pid, 1234U);
        EXPECT_EQ(record->unix_ms, 10000U);
        EXPECT_EQ(record->guest_hwnd, 4660U);
        EXPECT_EQ(record->identity, identity);
        EXPECT_EQ(record->metrics, metrics);
    }

    TEST(GpuWindowTitleTest, RejectsEveryTruncatedPrefixAndExtraLine)
    {
        const auto bytes = encode_record();
        for (size_t length = 0; length < bytes.size(); ++length)
        {
            EXPECT_FALSE(ui::decode_gpu_window_title_record(std::string_view{bytes}.substr(0, length)).has_value()) << length;
        }
        EXPECT_FALSE(ui::decode_gpu_window_title_record(bytes + '\n').has_value());
        EXPECT_FALSE(ui::decode_gpu_window_title_record(bytes + "extra\n").has_value());
        auto crlf = bytes;
        crlf.insert(crlf.find('\n'), "\r");
        EXPECT_FALSE(ui::decode_gpu_window_title_record(crlf).has_value());
    }

    TEST(GpuWindowTitleTest, RejectsUnknownVersionAndMalformedHeader)
    {
        auto bytes = encode_record();
        bytes.replace(0, std::string_view{"SOGEN_GPU_TITLE_V1"}.size(), "SOGEN_GPU_TITLE_V2");
        EXPECT_FALSE(ui::decode_gpu_window_title_record(bytes).has_value());
        EXPECT_FALSE(ui::decode_gpu_window_title_record(" " + encode_record()).has_value());
        EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("1234 ")).has_value());
        EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("1234", "10000 ")).has_value());
        EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("1234", "10000", "4660 ")).has_value());
    }

    TEST(GpuWindowTitleTest, RejectsInvalidUnsignedNumbersAndZeroTarget)
    {
        constexpr std::array invalid{"", "-1", "+1", " 1", "1 ", "1x", "0x123", "1\t2", "18446744073709551616"};
        for (const auto* value : invalid)
        {
            EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record(value)).has_value()) << value;
            EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("1234", value)).has_value()) << value;
            EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("1234", "10000", value)).has_value()) << value;
        }
        EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("4294967296")).has_value());
        EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("0")).has_value());
        EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("1234", "10000", "0")).has_value());
        EXPECT_TRUE(ui::decode_gpu_window_title_record(encode_record("1234", "0")).has_value());
    }

    TEST(GpuWindowTitleTest, AcceptsMaximumIntegerWidthsWithoutTruncation)
    {
        const auto record = ui::decode_gpu_window_title_record(encode_record("4294967295", "18446744073709551615", "18446744073709551615"));
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(record->host_pid, std::numeric_limits<uint32_t>::max());
        EXPECT_EQ(record->unix_ms, std::numeric_limits<uint64_t>::max());
        EXPECT_EQ(record->guest_hwnd, std::numeric_limits<uint64_t>::max());
        EXPECT_TRUE(ui::compose_gpu_window_title("Game", *record, std::numeric_limits<uint32_t>::max(),
                                                 std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max())
                        .has_value());
    }

    TEST(GpuWindowTitleTest, EnforcesByteLimitIncludingHeaderAndFinalNewline)
    {
        const auto padding = ui::gpu_window_title_max_record_bytes - encode_record().size();
        const auto long_identity = std::string{identity} + std::string(padding, 'A');
        const auto boundary = encode_record("1234", "10000", "4660", long_identity);
        ASSERT_EQ(boundary.size(), ui::gpu_window_title_max_record_bytes);
        EXPECT_TRUE(ui::decode_gpu_window_title_record(boundary).has_value());
        EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("1234", "10000", "4660", long_identity + 'A')).has_value());
    }

    TEST(GpuWindowTitleTest, RejectsEmbeddedControlBytesWithoutCStringTruncation)
    {
        for (uint32_t byte = 0; byte <= 0x7F; ++byte)
        {
            if (byte >= 0x20 && byte != 0x7F)
            {
                continue;
            }
            const auto suffix = std::string(1, static_cast<char>(byte));
            EXPECT_FALSE(
                ui::decode_gpu_window_title_record(encode_record("1234", "10000", "4660", std::string{identity} + suffix)).has_value())
                << byte;
            EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("1234", "10000", "4660", identity, std::string{metrics} + suffix))
                             .has_value())
                << byte;
        }
        auto header_nul = encode_record();
        header_nul.insert(header_nul.find(' '), 1, '\0');
        EXPECT_FALSE(ui::decode_gpu_window_title_record(header_nul).has_value());
        auto trailing_nul = encode_record();
        trailing_nul += '\0';
        EXPECT_FALSE(ui::decode_gpu_window_title_record(trailing_nul).has_value());
    }

    TEST(GpuWindowTitleTest, PreservesValidUtf8AndRejectsInvalidSequences)
    {
        const auto unicode_identity = std::string{identity} + " \xE2\x98\x83 \xF0\x9F\x8E\xAE";
        const auto record = ui::decode_gpu_window_title_record(encode_record("1234", "10000", "4660", unicode_identity));
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(record->identity, unicode_identity);
        constexpr std::array invalid{"\x80",         "\xC0\xAF",         "\xE2\x82",         "\xE2\x28\xA1",
                                     "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xC2\x85",
                                     "\xE2\x80\xA8", "\xE2\x80\xA9"};
        for (const auto* value : invalid)
        {
            EXPECT_FALSE(
                ui::decode_gpu_window_title_record(encode_record("1234", "10000", "4660", std::string{identity} + value)).has_value());
            EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("1234", "10000", "4660", identity, std::string{metrics} + value))
                             .has_value());
        }
    }

    TEST(GpuWindowTitleTest, RequiresNonemptyIdentityAndMetricFields)
    {
        constexpr std::array invalid_identity{
            "", "Type: discrete", "Adapter: GPU", "Type: | Adapter: GPU", "Type: discrete | Adapter: ", "Type: discrete | Adapter GPU"};
        for (const auto* value : invalid_identity)
        {
            EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("1234", "10000", "4660", value)).has_value());
        }
        constexpr std::array invalid_metrics{"",
                                             "Util: 75%",
                                             "Util: | VRAM: 1 | Mem: 2",
                                             "Util: 75% | VRAM: | Mem: 2",
                                             "Util: 75% | VRAM: 1 | Mem: ",
                                             "Util: 75% | Mem: 2 | VRAM: 1"};
        for (const auto* value : invalid_metrics)
        {
            EXPECT_FALSE(ui::decode_gpu_window_title_record(encode_record("1234", "10000", "4660", identity, value)).has_value());
        }
        EXPECT_TRUE(ui::decode_gpu_window_title_record(encode_record("1234", "10000", "4660", identity, "Util: N/A | VRAM: N/A | Mem: N/A"))
                        .has_value());
    }

    TEST(GpuWindowTitleTest, ComposesOnlyTheExactHostAndGuestWindow)
    {
        const auto record = ui::decode_gpu_window_title_record(encode_record());
        ASSERT_TRUE(record.has_value());
        EXPECT_FALSE(ui::compose_gpu_window_title("Game", *record, 1235, 4660, 10000).has_value());
        EXPECT_FALSE(ui::compose_gpu_window_title("Game", *record, 1234, 4661, 10000).has_value());
        EXPECT_FALSE(ui::compose_gpu_window_title("Game", *record, 0, 4660, 10000).has_value());
        EXPECT_FALSE(ui::compose_gpu_window_title("Game", *record, 1234, 0, 10000).has_value());
        EXPECT_FALSE(ui::compose_gpu_window_title("Game", *record, 1234, 0x100001234ULL, 10000).has_value());
    }

    TEST(GpuWindowTitleTest, FreshnessIncludesZeroThroughThreeThousandMilliseconds)
    {
        const auto record = ui::decode_gpu_window_title_record(encode_record());
        ASSERT_TRUE(record.has_value());
        const auto expected = "Game | " + std::string{identity} + " | " + std::string{metrics};
        EXPECT_EQ(ui::compose_gpu_window_title("Game", *record, 1234, 4660, 10000), expected);
        EXPECT_EQ(ui::compose_gpu_window_title("Game", *record, 1234, 4660, 12999), expected);
        EXPECT_EQ(ui::compose_gpu_window_title("Game", *record, 1234, 4660, 13000), expected);
    }

    TEST(GpuWindowTitleTest, StaleAndFutureSamplesNeverDisplayOldMetrics)
    {
        const auto record = ui::decode_gpu_window_title_record(encode_record());
        ASSERT_TRUE(record.has_value());
        const auto expected = "Game | " + std::string{identity} + " | Telemetry: stale";
        EXPECT_EQ(ui::compose_gpu_window_title("Game", *record, 1234, 4660, 13001), expected);
        EXPECT_EQ(ui::compose_gpu_window_title("Game", *record, 1234, 4660, 9999), expected);
        EXPECT_EQ(ui::compose_gpu_window_title("Game", *record, 1234, 4660, 0), expected);
        EXPECT_EQ(ui::compose_gpu_window_title("Game", *record, 1234, 4660, std::numeric_limits<uint64_t>::max()), expected);
        const auto future = ui::decode_gpu_window_title_record(encode_record("1234", "18446744073709551615"));
        ASSERT_TRUE(future.has_value());
        EXPECT_EQ(ui::compose_gpu_window_title("Game", *future, 1234, 4660, 1), expected);
    }

    TEST(GpuWindowTitleTest, PreservesGuestTitleExactlyWithoutParsingItsSuffix)
    {
        const auto record = ui::decode_gpu_window_title_record(encode_record());
        ASSERT_TRUE(record.has_value());
        const std::string base_title = "  Destiny \xE2\x98\x83 | Type: guest | Telemetry: stale  ";
        const auto expected = base_title + " | " + std::string{identity} + " | " + std::string{metrics};
        EXPECT_EQ(ui::compose_gpu_window_title(base_title, *record, 1234, 4660, 10000), expected);
        EXPECT_EQ(ui::compose_gpu_window_title("", *record, 1234, 4660, 10000), std::string{identity} + " | " + std::string{metrics});
    }

    TEST(GpuWindowTitleTest, RevalidatesManuallyConstructedOrModifiedRecords)
    {
        auto record = ui::decode_gpu_window_title_record(encode_record());
        ASSERT_TRUE(record.has_value());
        record->metrics += '\0';
        EXPECT_FALSE(ui::compose_gpu_window_title("Game", *record, 1234, 4660, 10000).has_value());
        record->metrics = metrics;
        record->identity.assign(ui::gpu_window_title_max_record_bytes, 'A');
        EXPECT_FALSE(ui::compose_gpu_window_title("Game", *record, 1234, 4660, 10000).has_value());
        record->identity = identity;
        record->guest_hwnd = 0;
        EXPECT_FALSE(ui::compose_gpu_window_title("Game", *record, 1234, 0, 10000).has_value());
        record->guest_hwnd = 4660;
        record->host_pid = 0;
        EXPECT_FALSE(ui::compose_gpu_window_title("Game", *record, 0, 4660, 10000).has_value());
    }
}
