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

    struct destiny_reason175_capture_state
    {
        static constexpr size_t site_count = 4;
        static constexpr uint64_t total_ms = 60000;
        static constexpr uint64_t closure_ms = 2000;
        static constexpr size_t max_records = 64;
        static constexpr size_t max_bytes = 64 * 1024;
        static constexpr size_t coverage_reserve = 2 * site_count + 5;
        static constexpr uint64_t hit_limit = 32;
        static constexpr uint64_t callback_limit_ns = 50000000;
        static constexpr size_t output_line_bytes = 2048 + destiny_startup_capture_budget::logger_envelope_bytes;
        static constexpr std::array<uint64_t, site_count> rvas{0xD3EB48, 0xE2DEB0, 0xE1B4D0, 0xE1B0CF};
        static constexpr std::array<size_t, site_count> lengths{2, 5, 5, 6};
        static constexpr std::array<std::array<uint8_t, 6>, site_count> bytes{
            std::array<uint8_t, 6>{0x33, 0xC9}, std::array<uint8_t, 6>{0x48, 0x89, 0x5C, 0x24, 0x18},
            std::array<uint8_t, 6>{0x48, 0x89, 0x5C, 0x24, 0x20}, std::array<uint8_t, 6>{0x0F, 0x84, 0x11, 0x01, 0x00, 0x00}};

        struct dependency_guard
        {
            uint64_t rva;
            size_t length;
            std::array<uint8_t, 75> bytes;
        };

        static constexpr std::array<dependency_guard, 15> dependencies{
            {dependency_guard{.rva = 0xE2DEB0, .length = 23, .bytes = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
                                                                       0x48, 0x8D, 0xAC, 0x24, 0xD0, 0xFC, 0xFF, 0xFF,
                                                                       0x48, 0x81, 0xEC, 0x30, 0x04, 0x00, 0x00}},
             dependency_guard{
                 .rva = 0xE2E07B, .length = 13, .bytes = {0x44, 0x8B, 0xC6, 0x8B, 0xD7, 0x48, 0x8B, 0xCB, 0xE8, 0x48, 0xD4, 0xFE, 0xFF}},
             dependency_guard{.rva = 0xD3EB10,
                              .length = 75,
                              .bytes = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xB9, 0x03, 0x00, 0x00, 0x00, 0xC7, 0x02, 0xFF, 0xFF,
                                        0xFF, 0xFF, 0x48, 0x8B, 0xDA, 0xE8, 0x27, 0x9A, 0xE0, 0xFF, 0x84, 0xC0, 0x74, 0x1B, 0xE8,
                                        0x3E, 0xEB, 0x0E, 0x00, 0x89, 0x03, 0x83, 0xF8, 0xFF, 0x75, 0x0F, 0xE8, 0x12, 0x24, 0xFD,
                                        0xFF, 0x84, 0xC0, 0x75, 0x06, 0xC7, 0x03, 0xAF, 0x00, 0x00, 0x00, 0x33, 0xC9, 0xB8, 0x02,
                                        0x00, 0x00, 0x00, 0x83, 0x3B, 0xFF, 0x0F, 0x44, 0xC1, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3}},
             dependency_guard{.rva = 0xD487E0, .length = 51, .bytes = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48,
                                                                       0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56,
                                                                       0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8D, 0x35, 0xFF, 0x77, 0x2B, 0xFF,
                                                                       0x48, 0x63, 0xEA, 0x33, 0xDB, 0x41, 0xC7, 0x01, 0xFF, 0xFF, 0xFF,
                                                                       0xFF, 0x49, 0x8B, 0xF9, 0x49, 0x8B, 0xF0}},
             dependency_guard{.rva = 0xD4892C, .length = 10, .bytes = {0x48, 0x8B, 0xD7, 0xE8, 0xDC, 0x61, 0xFF, 0xFF, 0x8B, 0xD8}},
             dependency_guard{.rva = 0xD4D890, .length = 39, .bytes = {0x40, 0x55, 0x53, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xB0, 0xFC,
                                                                       0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x50, 0x04, 0x00, 0x00, 0x48,
                                                                       0x8B, 0x05, 0xDE, 0xC1, 0x35, 0x01, 0x48, 0x33, 0xC4, 0x48,
                                                                       0x89, 0x85, 0x40, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xF9}},
             dependency_guard{.rva = 0xD4D8D0, .length = 37, .bytes = {0x4C, 0x8D, 0x4C, 0x24, 0x30, 0xC7, 0x44, 0x24, 0x30, 0xFF,
                                                                       0xFF, 0xFF, 0xFF, 0x4C, 0x8D, 0x85, 0x40, 0x02, 0x00, 0x00,
                                                                       0xC6, 0x85, 0x40, 0x02, 0x00, 0x00, 0x00, 0x8B, 0xD3, 0x48,
                                                                       0x8B, 0xCF, 0xE8, 0xEB, 0xAE, 0xFF, 0xFF}},
             dependency_guard{.rva = 0xD4DB7B,
                              .length = 16,
                              .bytes = {0x8B, 0xD3, 0x89, 0x5C, 0x24, 0x30, 0xB9, 0x1C, 0x00, 0x00, 0x00, 0xE8, 0x25, 0x03, 0x0E, 0x00}},
             dependency_guard{.rva = 0x1071F85, .length = 25, .bytes = {0x48, 0x63, 0x48, 0x20, 0x48, 0x39, 0x4E, 0x08, 0x7C,
                                                                        0x21, 0xBA, 0xAF, 0x00, 0x00, 0x00, 0xB9, 0x1C, 0x00,
                                                                        0x00, 0x00, 0xE8, 0x12, 0xBF, 0xDB, 0xFF}},
             dependency_guard{.rva = 0xE1B4D0, .length = 5, .bytes = {0x48, 0x89, 0x5C, 0x24, 0x20}},
             dependency_guard{
                 .rva = 0xE1AE70,
                 .length = 18,
                 .bytes = {0x40, 0x55, 0x53, 0x48, 0x8D, 0xAC, 0x24, 0xE8, 0xFD, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x18, 0x03, 0x00, 0x00}},
             dependency_guard{.rva = 0xE1B094, .length = 65, .bytes = {0x8B, 0x83, 0xA0, 0x03, 0x00, 0x00, 0x89, 0x83, 0x90, 0x03, 0x00,
                                                                       0x00, 0x8B, 0x83, 0xA4, 0x03, 0x00, 0x00, 0x89, 0x83, 0x94, 0x03,
                                                                       0x00, 0x00, 0x80, 0x3D, 0x6D, 0x88, 0x89, 0x01, 0x00, 0x48, 0x8B,
                                                                       0x05, 0x6E, 0x88, 0x89, 0x01, 0x75, 0x05, 0xE8, 0x8F, 0x35, 0x4E,
                                                                       0xFF, 0x83, 0xBB, 0xA0, 0x03, 0x00, 0x00, 0xFF, 0x48, 0x89, 0x83,
                                                                       0x98, 0x03, 0x00, 0x00, 0x0F, 0x84, 0x11, 0x01, 0x00, 0x00}},
             dependency_guard{.rva = 0xE23563, .length = 5, .bytes = {0xE8, 0x08, 0x79, 0xFF, 0xFF}},
             dependency_guard{.rva = 0xD10F50, .length = 34, .bytes = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x0D, 0x95, 0x66,
                                                                       0x2A, 0x01, 0xE8, 0x60, 0xDD, 0xEC, 0xFF, 0x48, 0x8B,
                                                                       0xC8, 0xE8, 0xA8, 0xD9, 0xEC, 0xFF, 0x84, 0xC0, 0x0F,
                                                                       0x94, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3}},
             dependency_guard{.rva = 0xBDE910, .length = 8, .bytes = {0x0F, 0xB6, 0x81, 0x28, 0x07, 0x00, 0x00, 0xC3}}}};

        static bool dependency_matches(size_t dependency, const uint8_t* data, size_t length);
        static bool callback_exhausted(uint64_t hits, uint64_t callback_ns);

        static bool guard_matches(size_t site, const uint8_t* data, size_t length);
        static bool quiescent(const bool* running, size_t count, size_t executing);
        static bool read_complete(uint64_t mask, uint64_t required);
        static bool record_allowed(size_t accepted, size_t pending, bool coverage);
        static bool output_allowed(size_t records, size_t output_bytes, size_t charged_bytes, bool coverage);

        void arm(uint64_t now);
        std::string_view expiry(uint64_t now) const;
        bool observe_request(uint32_t tid, uint32_t reason, uint64_t rsp, uint64_t caller, uint64_t now, uint64_t sequence);
        bool observe_setter(uint32_t tid, uint32_t target, uint32_t reason, uint64_t manager, uint64_t wrapper_rsp, uint64_t wrapper_caller,
                            bool frame_valid);
        bool observe_commit(uint64_t manager, uint32_t current, uint32_t current_reason, uint32_t goal, uint32_t goal_reason,
                            bool frame_valid);
        bool complete(uint64_t drops, uint64_t failed_reads) const;

        uint64_t armed_at{};
        uint64_t first_at{};
        uint64_t request_rsp{};
        uint64_t request_caller{};
        uint64_t first_sequence{};
        uint64_t manager{};
        uint32_t tid{};
        uint32_t reason{};
        uint8_t source{};
        bool armed{};
        bool first_seen{};
        bool setter_seen{};
        bool commit_seen{};
        bool ambiguous{};
        bool producer_verified{};
    };

    class destiny_startup_capture : public std::enable_shared_from_this<destiny_startup_capture>
    {
      public:
        static bool enabled();
        static bool reason175_enabled();
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
        void arm_reason175_locked(uint64_t now);
        void capture_reason175(cpu_interface& cpu, uint64_t rip, size_t site);
        void reason175_summary_locked(uint64_t now);
        size_t selected_site_count() const;
        size_t record_limit() const;
        size_t byte_limit() const;
        size_t coverage_reserve() const;
        std::string_view capture_expiry(uint64_t now) const;
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
        destiny_reason175_capture_state reason175_{};

        struct blocked_reason_witness
        {
            uint64_t reason_pointer{};
            uint64_t owner{};
            uint64_t outer_return{};
            uint32_t tid{};
            bool seen{};
            bool valid{};
        } blocked_reason_{};

        bool focused_{reason175_enabled()};
        bool reason175_summary_seen_{};
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
