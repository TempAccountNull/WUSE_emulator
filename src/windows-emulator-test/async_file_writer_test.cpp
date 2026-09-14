#include "std_include.hpp"

#include <gtest/gtest.h>
#include <utils/async_file_writer.hpp>
#include <utils/finally.hpp>
#include <atomic>
#include <fstream>
#include <iterator>

namespace sogen
{
    namespace
    {
        std::filesystem::path writer_test_path()
        {
            static std::atomic_uint64_t sequence{};
            return std::filesystem::temp_directory_path() /
                   ("sogen-writer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                    std::to_string(sequence.fetch_add(1)) + ".log");
        }

        std::string read_output(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        }
    }

    TEST(AsyncFileWriter, PreservesOrderedRecordsAcrossBufferBoundaries)
    {
        const auto path = writer_test_path();
        const auto cleanup = utils::finally([&] { std::filesystem::remove(path); });
        std::string expected;
        utils::async_file_writer writer(path);
        for (size_t i = 0; i < 50000; ++i)
        {
            const auto record = "{\"sequence\":" + std::to_string(i) + ",\"payload\":\"abcdefghijklmnopqrstuvwxyz\"}\n";
            expected += record;
            writer.write(record);
        }
        writer.flush();
        EXPECT_EQ(read_output(path), expected);
        writer.write("last\n");
        writer.flush();
        EXPECT_EQ(read_output(path), expected + "last\n");
    }

    TEST(AsyncFileWriter, DrainsOversizedRecordOnDestruction)
    {
        const auto path = writer_test_path();
        const auto cleanup = utils::finally([&] { std::filesystem::remove(path); });
        std::string record(3 * 1024 * 1024 + 17, 'x');
        record[1024 * 1024] = '\0';
        {
            utils::async_file_writer writer(path);
            writer.write(record);
        }
        EXPECT_EQ(read_output(path), record);
    }

    TEST(AsyncFileWriter, FlushesSparseOutputWhileIdle)
    {
        const auto path = writer_test_path();
        const auto cleanup = utils::finally([&] { std::filesystem::remove(path); });
        utils::async_file_writer writer(path);
        writer.write("progress\n");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (read_output(path) != "progress\n" && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_EQ(read_output(path), "progress\n");
    }

    TEST(AsyncFileWriter, RejectsUnwritableOutput)
    {
        EXPECT_THROW((utils::async_file_writer{std::filesystem::temp_directory_path()}), std::runtime_error);
    }
}
