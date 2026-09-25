#include "../windows-emulator/telemetry_shared_memory.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"
#include "../windows-analyzer/jsonl_reporter.hpp"

#include <gtest/gtest.h>
#include <utils/finally.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
namespace sogen::test
{
    TEST(TelemetrySharedMemory, PublishesBoundedSnapshotAndReleasesMapping)
    {
        const auto name = std::wstring(L"Local\\SogenTelemetry-") + std::to_wstring(GetCurrentProcessId());
        {
            detail::telemetry_shared_memory publisher;
            constexpr std::string_view json = R"({"activity":1})";
            ASSERT_TRUE(publisher.publish(json));

            auto* const mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
            ASSERT_NE(mapping, nullptr);
            auto* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, detail::telemetry_shared_memory::mapping_size);
            ASSERT_NE(view, nullptr);
            auto* const bytes = static_cast<const unsigned char*>(view);
            const auto* const header = reinterpret_cast<const uint32_t*>(bytes);
            EXPECT_EQ(header[0], 2u);
            EXPECT_EQ(header[1], json.size());
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes + detail::telemetry_shared_memory::header_size), header[1]),
                      json);

            const std::string oversized(detail::telemetry_shared_memory::max_payload_size + 1, 'x');
            EXPECT_FALSE(publisher.publish(oversized));
            EXPECT_EQ(header[0], 2u);
            EXPECT_EQ(header[1], json.size());

            EXPECT_TRUE(UnmapViewOfFile(view));
            EXPECT_TRUE(CloseHandle(mapping));
        }
        auto* const released = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        EXPECT_EQ(released, nullptr);
        if (released)
        {
            CloseHandle(released);
        }
    }

    TEST(TelemetrySharedMemory, StatusOnlyReporterPublishesAnalysisMapping)
    {
        const auto* previous = std::getenv("SOGEN_TELEMETRY_SHM");
        const std::string previous_value = previous ? previous : "";
        const bool had_previous = previous != nullptr;
        ASSERT_EQ(_putenv_s("SOGEN_TELEMETRY_SHM", "1"), 0);

        const auto name = std::wstring(L"Local\\SogenAnalysis-") + std::to_wstring(GetCurrentProcessId());
        {
            auto reporter = create_jsonl_reporter({}, {});
            auto* const mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
            ASSERT_NE(mapping, nullptr);
            auto* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, detail::telemetry_shared_memory::mapping_size);
            ASSERT_NE(view, nullptr);
            auto* const bytes = static_cast<const unsigned char*>(view);
            const auto* const header = reinterpret_cast<const uint32_t*>(bytes);
            EXPECT_EQ(header[0] & 1u, 0u);
            ASSERT_LE(header[1], detail::telemetry_shared_memory::max_payload_size);
            const std::string_view json(reinterpret_cast<const char*>(bytes + detail::telemetry_shared_memory::header_size), header[1]);
            EXPECT_NE(json.find("\"mode\":\"status\""), std::string_view::npos);
            EXPECT_NE(json.find("\"observed_events\":\"0\""), std::string_view::npos);
            EXPECT_TRUE(UnmapViewOfFile(view));
            EXPECT_TRUE(CloseHandle(mapping));
        }

        EXPECT_EQ(OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str()), nullptr);
        ASSERT_EQ(_putenv_s("SOGEN_TELEMETRY_SHM", had_previous ? previous_value.c_str() : ""), 0);
    }

    TEST(TelemetrySharedMemory, CombinedStatusPublishesFinalSnapshotToFileAndMapping)
    {
        const auto* previous = std::getenv("SOGEN_TELEMETRY_SHM");
        const std::string previous_value = previous ? previous : "";
        const bool had_previous = previous != nullptr;
        ASSERT_EQ(_putenv_s("SOGEN_TELEMETRY_SHM", "1"), 0);
        const auto restore_environment =
            utils::finally([&] { _putenv_s("SOGEN_TELEMETRY_SHM", had_previous ? previous_value.c_str() : ""); });

        const auto status =
            std::filesystem::temp_directory_path() / ("sogen-analysis-combined-status-" + std::to_string(GetCurrentProcessId()) + ".json");
        const auto remove_status = utils::finally([&] {
            std::error_code error;
            std::filesystem::remove(status, error);
        });
        std::error_code error;
        std::filesystem::remove(status, error);

        jsonl_report_settings settings{};
        settings.status_path = status;
        auto reporter = create_jsonl_reporter({}, settings);
        reporter->report(run_started_event{});
        reporter->flush();

        std::ifstream status_stream(status, std::ios::binary);
        ASSERT_TRUE(status_stream.is_open());
        const std::string file_json(std::istreambuf_iterator<char>{status_stream}, std::istreambuf_iterator<char>{});
        EXPECT_NE(file_json.find("\"observed_events\":\"1\""), std::string::npos);

        const auto name = std::wstring(L"Local\\SogenAnalysis-") + std::to_wstring(GetCurrentProcessId());
        auto* const mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        ASSERT_NE(mapping, nullptr);
        const auto close_mapping = utils::finally([&] { CloseHandle(mapping); });
        auto* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, detail::telemetry_shared_memory::mapping_size);
        ASSERT_NE(view, nullptr);
        const auto unmap_view = utils::finally([&] { UnmapViewOfFile(view); });
        const auto* const bytes = static_cast<const unsigned char*>(view);
        const auto* const header = reinterpret_cast<const uint32_t*>(bytes);
        ASSERT_LE(header[1], detail::telemetry_shared_memory::max_payload_size);
        const std::string_view mapped_json(reinterpret_cast<const char*>(bytes + detail::telemetry_shared_memory::header_size), header[1]);
        EXPECT_NE(mapped_json.find("\"observed_events\":\"1\""), std::string_view::npos);
    }
}
#endif
