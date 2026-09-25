#include "../windows-emulator/file_read_profile.hpp"

#include <gtest/gtest.h>
#include <thread>

namespace sogen::test
{
    TEST(FileReadProfile, ConcurrentPackageAndOtherReadsRemainSeparate)
    {
        file_read_profile profile(true);
        const file_read_profile::sample package_sample{4096, 2048, 10, 20, 30, 40, 120};
        const file_read_profile::sample other_sample{512, 256, 1, 2, 3, 4, 12};

        std::thread package_reader([&] {
            for (int i = 0; i < 10000; ++i)
            {
                profile.record(true, package_sample);
            }
        });
        std::thread other_reader([&] {
            for (int i = 0; i < 10000; ++i)
            {
                profile.record(false, other_sample);
            }
        });
        package_reader.join();
        other_reader.join();

        const auto result = profile.read();
        EXPECT_EQ(result.package.calls, 10000);
        EXPECT_EQ(result.package.requested_bytes, 40960000);
        EXPECT_EQ(result.package.read_bytes, 20480000);
        EXPECT_EQ(result.package.allocation_nanos, 100000);
        EXPECT_EQ(result.package.seek_nanos, 200000);
        EXPECT_EQ(result.package.host_read_nanos, 300000);
        EXPECT_EQ(result.package.guest_write_nanos, 400000);
        EXPECT_EQ(result.package.total_nanos, 1200000);
        EXPECT_EQ(result.other.calls, 10000);
        EXPECT_EQ(result.other.requested_bytes, 5120000);
        EXPECT_EQ(result.other.read_bytes, 2560000);
        EXPECT_EQ(result.other.total_nanos, 120000);
    }

    TEST(FileReadProfile, DisabledProfileDoesNotRecord)
    {
        file_read_profile profile(false);
        profile.record(true, {.requested_bytes = 4096, .read_bytes = 2048});
        const auto result = profile.read();
        EXPECT_EQ(result.package.calls, 0);
        EXPECT_EQ(result.other.calls, 0);
    }
}
