#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>

namespace sogen
{
    class windows_emulator;
    class kernel_lock;
    struct emulator_hook;
    struct cpu_interface;
    struct mapped_module;

    struct destiny_startup_capture_budget
    {
        static constexpr uint64_t total_ms = 300000;
        static constexpr uint64_t post_cleanup_ms = 120000;
        static constexpr size_t max_records = 1024;
        static constexpr size_t max_bytes = 1024 * 1024;
        static constexpr size_t coverage_reserve = 83;
        static constexpr size_t logger_envelope_bytes = 9;

        uint64_t armed_at{};
        uint64_t cleanup_at{};
        bool armed{};
        bool cleanup_seen{};

        void arm(uint64_t now);
        void observe_cleanup(uint64_t now);
        std::string_view expiry(uint64_t now) const;
    };

    class destiny_startup_capture : public std::enable_shared_from_this<destiny_startup_capture>
    {
      public:
        static bool enabled();
        static std::shared_ptr<destiny_startup_capture> create(windows_emulator& owner, kernel_lock& kernel, uint64_t base,
                                                               uint64_t image_size);
        ~destiny_startup_capture();
        void shutdown();

      private:
        static constexpr size_t site_count = 39;
        static constexpr size_t thread_count = 32;
        static constexpr size_t value_count = 12;

        struct record
        {
            uint64_t host_ms{};
            uint64_t rip{};
            uint64_t token{};
            uint32_t tid{};
            uint32_t vcpu{};
            uint8_t kind{};
            uint8_t site{};
            uint8_t recorder{};
            uint8_t length{};
            uint64_t flags{};
            std::array<uint64_t, value_count> values{};
            std::array<uint8_t, 64> bytes{};
        };

        struct site_state
        {
            std::atomic<uint64_t> raw_traps{};
            emulator_hook* hook{};
            uint64_t hits{};
            uint64_t identity_misses{};
            uint64_t read_failures{};
            uint64_t signature_misses{};
            uint64_t dropped{};
            uint64_t emitted{};
            uint64_t callback_ns{};
            uint64_t callback_max_ns{};
            uint8_t retirement{};
            bool attempted{};
            bool last_valid{};
            record last{};
        };

        struct resource_state
        {
            uint32_t handle{0xFFFFFFFF};
            uint32_t child{0xFFFFFFFF};
            uint32_t tag{};
            uint64_t token{};
            uint64_t wrapper{};
            uint64_t slot{};
            uint64_t owner{};
            uint32_t tid{};
            uint32_t terminal_status{};
            uint64_t pre_free_rsp{};
            uint64_t world_owner{};
            uint64_t world_context{};
            uint16_t transaction{0xFFFF};
            bool native_assignment{};
            bool pre_free{};
            std::array<uint8_t, 0x44> wrapper_bytes{};
        };

        struct sender_state
        {
            uint32_t tid{};
            uint8_t stage{};
            uint64_t token{};
            uint64_t manager{};
            uint64_t record_address{};
            uint64_t identity{};
            uint64_t pump_rsp{};
            uint64_t dispatch_rsp{};
            uint64_t descriptor{};
            uint64_t completion_token{};
            uint32_t dispatch_mode{};
            std::array<uint8_t, 48> bytes{};
        };

        struct request_view
        {
            uint64_t address{};
            std::array<uint8_t, 0x44> bytes{};
        };

        destiny_startup_capture(windows_emulator& owner, kernel_lock& kernel, uint64_t base, uint64_t image_size);
        void start();
        void observe_debug(std::string_view message);
        void observe_unload(mapped_module& module);
        void arm_locked(uint64_t now);
        bool verify_cleanup_guards_locked(uint64_t now);
        void baseline_locked(uint8_t phase, uint64_t now);
        void capture(cpu_interface& cpu, uint64_t rip, size_t site);
        bool resolve_request(uint32_t handle, request_view& view, record& observation);
        bool read(uint64_t address, void* destination, size_t length, record& observation);
        bool read_offset(uint64_t address, uint64_t offset, void* destination, size_t length, record& observation);
        bool enqueue_locked(record observation, bool coverage = false);
        void retire_locked(uint8_t reason, uint64_t now);
        bool quiescent_locked(size_t executing_vcpu) const;
        void remove_hooks_locked(size_t executing_vcpu = static_cast<size_t>(-1));
        void retire_site_locked(size_t index, uint8_t reason, size_t executing_vcpu);
        void maintenance(const std::stop_token& stop);
        void output_worker(const std::stop_token& stop);
        void drain();
        sender_state* sender_locked(uint32_t tid, bool allocate);
        uint64_t now_ms() const;

        windows_emulator& owner_;
        kernel_lock& kernel_;
        uint64_t base_{};
        uint64_t image_size_{};
        std::chrono::steady_clock::time_point origin_{std::chrono::steady_clock::now()};
        std::mutex mutex_{};
        std::condition_variable_any wake_{};
        destiny_startup_capture_budget budget_{};
        std::array<site_state, site_count> sites_{};
        std::array<resource_state, 2> resources_{};
        std::array<sender_state, thread_count> senders_{};
        std::array<record, destiny_startup_capture_budget::max_records> records_{};
        size_t pending_begin_{};
        size_t pending_count_{};
        size_t accepted_{};
        size_t output_records_{};
        size_t output_bytes_{};
        uint64_t serial_{};
        uint64_t dropped_{};
        uint64_t host_remove_attempts_{};
        uint64_t last_snapshot_{};
        uint64_t snapshots_{};
        uint64_t snapshot_misses_{};
        uint64_t read_requests_{};
        uint64_t failed_read_requests_{};
        uint64_t retired_traps_{};
        uint64_t quiescence_deferrals_{};
        uint8_t retired_{};
        bool baseline_seen_{};
        bool snapshot_seen_{};
        bool physical_retirement_{};
        bool final_receipt_{};
        bool dispatcher_frame_verified_{};
        bool pool_layout_verified_{};
        bool cleanup_guards_verified_{};
        bool shutdown_{};
        std::jthread worker_{};
        std::jthread output_worker_{};
    };
}
