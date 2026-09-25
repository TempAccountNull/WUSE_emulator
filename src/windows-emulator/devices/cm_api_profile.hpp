#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>

namespace sogen {

// Optional, bounded aggregate for the high-volume CMApi interface-list query.
// No individual calls are logged and the disabled path takes no timer or lock.
class cm_api_interface_profile {
 public:
  static constexpr size_t guid_capacity = 64;

  struct bucket {
    uint64_t calls{};
    uint64_t nanos{};
    uint64_t max_nanos{};
  };

  struct guid_bucket {
    std::array<uint8_t, 16> guid{};
    bucket timing{};
    // all registered, active only, invalid flags; unparsed requests have no GUID.
    std::array<uint64_t, 3> calls_by_flags{};
  };

  struct snapshot {
    // all registered, active only, invalid flags, unreadable request
    std::array<bucket, 4> by_flags{};
    std::array<guid_bucket, guid_capacity> by_guid{};
    uint32_t tracked_unique_guids{};
    uint64_t untracked_guid_calls{};
    uint64_t untracked_guid_nanos{};
  };

  explicit cm_api_interface_profile(bool enabled = env_enabled())
      : enabled_(enabled) {}

  bool enabled() const noexcept { return enabled_; }

  void record(std::optional<uint32_t> flags,
              const std::array<uint8_t, 16>& guid,
              uint64_t elapsed_nanos) {
    if (!enabled_) return;
    const size_t category = !flags ? 3 : *flags == 0 ? 0 : *flags == 0x10000 ? 1 : 2;
    std::lock_guard lock(mutex_);
    update(data_.by_flags[category], elapsed_nanos);
    if (!flags) return;

    for (uint32_t i = 0; i < data_.tracked_unique_guids; ++i) {
      auto& entry = data_.by_guid[i];
      if (entry.guid == guid) {
        update(entry.timing, elapsed_nanos);
        ++entry.calls_by_flags[category];
        return;
      }
    }
    if (data_.tracked_unique_guids < guid_capacity) {
      auto& entry = data_.by_guid[data_.tracked_unique_guids++];
      entry.guid = guid;
      update(entry.timing, elapsed_nanos);
      ++entry.calls_by_flags[category];
    } else {
      ++data_.untracked_guid_calls;
      data_.untracked_guid_nanos += elapsed_nanos;
    }
  }

  snapshot read() const {
    std::lock_guard lock(mutex_);
    return data_;
  }

 private:
  static void update(bucket& target, uint64_t elapsed_nanos) {
    ++target.calls;
    target.nanos += elapsed_nanos;
    target.max_nanos = std::max(target.max_nanos, elapsed_nanos);
  }

  static bool env_enabled() {
    const char* value = std::getenv("SOGEN_PROFILE_CMAPI_INTERFACE_LIST");
    return value && *value && *value != '0';
  }

  const bool enabled_;
  mutable std::mutex mutex_;
  snapshot data_{};
};

}  // namespace sogen