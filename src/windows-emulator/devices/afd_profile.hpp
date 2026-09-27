#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace sogen {

// Opt-in, fixed-size counters. No guest addresses, payloads, or per-call records.
class afd_profile {
 public:
  enum class operation : size_t {
    connect,
    send,
    receive,
    send_datagram,
    receive_datagram,
    poll_request,
    count
  };

  struct bucket {
    uint64_t attempts{};
    uint64_t retries{};
    uint64_t success{};
    uint64_t pending{};
    uint64_t not_supported{};
    uint64_t device_not_ready{};
    uint64_t timeout{};
    uint64_t other_failure{};
    uint64_t exceptions{};
    uint64_t multi_buffer_calls{};
    uint64_t max_buffer_count{};
    uint64_t transferred_bytes{};
    uint64_t max_transfer_bytes{};
    uint64_t io_nanos{};
    uint64_t max_io_nanos{};
    uint64_t completions{};
    uint64_t completion_timeouts{};
    uint64_t completion_nanos{};
    uint64_t max_completion_nanos{};
    uint32_t last_status{};
  };

  static constexpr size_t operation_count = static_cast<size_t>(operation::count);
  using snapshot = std::array<bucket, operation_count>;

  explicit afd_profile(bool enabled = env_enabled()) : enabled_(enabled) {}

  bool enabled() const noexcept { return enabled_; }

  void record_attempt(operation kind, uint32_t status, uint64_t nanos, bool retry) {
    if (!enabled_) return;
    std::lock_guard lock(mutex_);
    auto& entry = data_[index(kind)];
    ++entry.attempts;
    if (retry) ++entry.retries;
    entry.io_nanos += nanos;
    entry.max_io_nanos = std::max(entry.max_io_nanos, nanos);
    entry.last_status = status;
    if (status == 0) ++entry.success;
    else if (status == 0x103) ++entry.pending;                 // STATUS_PENDING
    else if (status == 0xC00000BB) ++entry.not_supported;     // STATUS_NOT_SUPPORTED
    else if (status == 0xC00000A3) ++entry.device_not_ready;  // STATUS_DEVICE_NOT_READY
    else if (status == 0x102) ++entry.timeout;                // STATUS_TIMEOUT
    else ++entry.other_failure;
  }

  void record_exception(operation kind, uint64_t nanos, bool retry) {
    if (!enabled_) return;
    std::lock_guard lock(mutex_);
    auto& entry = data_[index(kind)];
    ++entry.attempts;
    if (retry) ++entry.retries;
    ++entry.exceptions;
    entry.io_nanos += nanos;
    entry.max_io_nanos = std::max(entry.max_io_nanos, nanos);
  }

  void record_buffer_count(operation kind, uint32_t count) {
    if (!enabled_) return;
    std::lock_guard lock(mutex_);
    auto& entry = data_[index(kind)];
    if (count > 1) ++entry.multi_buffer_calls;
    entry.max_buffer_count = std::max(entry.max_buffer_count, static_cast<uint64_t>(count));
  }

  void record_transfer(operation kind, uint64_t bytes) {
    if (!enabled_) return;
    std::lock_guard lock(mutex_);
    auto& entry = data_[index(kind)];
    entry.transferred_bytes += bytes;
    entry.max_transfer_bytes = std::max(entry.max_transfer_bytes, bytes);
  }

  void record_completion(operation kind, uint32_t status, uint64_t nanos) {
    if (!enabled_) return;
    std::lock_guard lock(mutex_);
    auto& entry = data_[index(kind)];
    ++entry.completions;
    if (status == 0x102) ++entry.completion_timeouts;
    entry.completion_nanos += nanos;
    entry.max_completion_nanos = std::max(entry.max_completion_nanos, nanos);
    entry.last_status = status;
  }

  snapshot read() const {
    std::lock_guard lock(mutex_);
    return data_;
  }

 private:
  static constexpr size_t index(operation kind) { return static_cast<size_t>(kind); }

  static bool env_enabled() {
    const char* value = std::getenv("SOGEN_PROFILE_AFD");
    return value && *value == '1' && value[1] == '\0';
  }

  const bool enabled_;
  mutable std::mutex mutex_;
  snapshot data_{};
};

}  // namespace sogen
