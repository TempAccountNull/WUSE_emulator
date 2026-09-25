#include "../windows-emulator/devices/cm_api_profile.hpp"

#include <gtest/gtest.h>
#include <thread>

namespace sogen::test {

TEST(CmApiInterfaceProfile, ConcurrentRequestsRemainBoundedAndCounted) {
  cm_api_interface_profile profile(true);
  std::array<uint8_t, 16> all_guid{};
  std::array<uint8_t, 16> active_guid{};
  all_guid[0] = 1;
  active_guid[0] = 2;

  std::thread all([&] {
    for (int i = 0; i < 1000; ++i) profile.record(0, all_guid, 10);
  });
  std::thread active([&] {
    for (int i = 0; i < 1000; ++i) profile.record(0x10000, active_guid, 20);
  });
  all.join();
  active.join();

  auto result = profile.read();
  EXPECT_EQ(result.by_flags[0].calls, 1000);
  EXPECT_EQ(result.by_flags[0].nanos, 10000);
  EXPECT_EQ(result.by_flags[1].calls, 1000);
  EXPECT_EQ(result.by_flags[1].nanos, 20000);
  ASSERT_EQ(result.tracked_unique_guids, 2);
  const auto& all_bucket = result.by_guid[0].guid == all_guid ? result.by_guid[0] : result.by_guid[1];
  const auto& active_bucket = result.by_guid[0].guid == active_guid ? result.by_guid[0] : result.by_guid[1];
  EXPECT_EQ(all_bucket.guid, all_guid);
  EXPECT_EQ(all_bucket.timing.calls, 1000);
  EXPECT_EQ(all_bucket.timing.nanos, 10000);
  EXPECT_EQ(all_bucket.timing.max_nanos, 10);
  EXPECT_EQ(all_bucket.calls_by_flags[0], 1000);
  EXPECT_EQ(active_bucket.guid, active_guid);
  EXPECT_EQ(active_bucket.timing.calls, 1000);
  EXPECT_EQ(active_bucket.timing.nanos, 20000);
  EXPECT_EQ(active_bucket.timing.max_nanos, 20);
  EXPECT_EQ(active_bucket.calls_by_flags[1], 1000);

  // Reuse a tracked class with another flag before filling the table.
  profile.record(0x10, all_guid, 30);
  result = profile.read();
  const auto& updated_all = result.by_guid[0].guid == all_guid ? result.by_guid[0] : result.by_guid[1];
  EXPECT_EQ(updated_all.timing.calls, 1001);
  EXPECT_EQ(updated_all.timing.nanos, 10030);
  EXPECT_EQ(updated_all.timing.max_nanos, 30);
  EXPECT_EQ(updated_all.calls_by_flags[2], 1);

  for (uint8_t i = 3; i <= 65; ++i) {
    std::array<uint8_t, 16> guid{};
    guid[0] = i;
    profile.record(0, guid, 5);
  }
  profile.record(std::nullopt, {}, 7);
  result = profile.read();
  EXPECT_EQ(result.tracked_unique_guids, cm_api_interface_profile::guid_capacity);
  EXPECT_EQ(result.untracked_guid_calls, 1);
  EXPECT_EQ(result.untracked_guid_nanos, 5);
  EXPECT_EQ(result.by_flags[0].calls, 1063);
  EXPECT_EQ(result.by_flags[2].calls, 1);
  EXPECT_EQ(result.by_flags[3].calls, 1);
  EXPECT_EQ(result.by_flags[3].nanos, 7);
}

TEST(CmApiInterfaceProfile, DisabledDoesNotCapture) {
  cm_api_interface_profile profile(false);
  std::array<uint8_t, 16> guid{};
  guid[0] = 1;
  profile.record(0, guid, 10);
  const auto result = profile.read();
  EXPECT_EQ(result.by_flags[0].calls, 0);
  EXPECT_EQ(result.tracked_unique_guids, 0);
}

}  // namespace sogen::test