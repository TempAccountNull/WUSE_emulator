#pragma once

#include "native_child_sample.hpp"
#include <hook_interface.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace sogen
{
    class windows_emulator;
    class kernel_lock;
    struct cpu_interface;
    struct emulator_hook;

    class native_child_capture : public std::enable_shared_from_this<native_child_capture>
    {
      public:
        static bool enabled();
        static std::shared_ptr<native_child_capture> create(windows_emulator& owner, kernel_lock& kernel,
                                                            const detail::native_marker_snapshot& marker);
        ~native_child_capture();
        bool shutdown() noexcept;
        bool reporting_stopped() const noexcept;
        bool snapshot_safe() const noexcept;

      private:
        friend struct native_child_capture_test_access;

        struct dependencies
        {
            std::function<std::jthread(std::function<void(std::stop_token)>)> start_worker{};
            std::function<emulator_hook*(uint32_t, uint64_t, hook_interface::memory_execution_hook_mode, memory_execution_hook_callback)>
                install_hook{};
        };

        static std::shared_ptr<native_child_capture> create(windows_emulator& owner, kernel_lock& kernel,
                                                            const detail::native_marker_snapshot& marker, dependencies operations);
        native_child_capture(windows_emulator& owner, kernel_lock& kernel, detail::native_child_selection selected,
                             dependencies operations);
        void capture(cpu_interface& cpu, uint64_t rip, uint32_t site);
        void service(size_t executing_cpu = SIZE_MAX);
        bool emit(detail::native_child_sample sample, bool lifecycle);
        void retire(uint32_t site, detail::native_child_reason reason, uint64_t now);
        bool quiescent(size_t executing_cpu) const;
        bool same_module() const;
        void maintenance(std::stop_token stop);
        static uint64_t now();

        windows_emulator& owner_;
        kernel_lock& kernel_;
        detail::native_child_selection selected_{};
        dependencies operations_{};
        std::array<emulator_hook*, 2> hooks_{};
        std::array<detail::native_child_site_counts, 2> counts_{};
        std::array<std::atomic<uint64_t>, 2> raw_traps_{};
        std::jthread worker_{};
        bool activated_{};
        bool shutting_down_{};
        bool removal_accepted_{true};
        bool final_sent_{};
        std::atomic<bool> emission_closed_{};
        uint64_t sequence_{};
        uint64_t last_ns_{};
        uint64_t collection_ns_{};
        uint64_t callback_ns_{};
        size_t detail_records_{};
        size_t lifecycle_records_{};
        size_t detail_bytes_{};
        size_t lifecycle_bytes_{};
        uint64_t output_rejections_{};
    };

    class native_child_capture_scope
    {
      public:
        explicit native_child_capture_scope(windows_emulator& owner);
        ~native_child_capture_scope();
        native_child_capture_scope(const native_child_capture_scope&) = delete;
        native_child_capture_scope& operator=(const native_child_capture_scope&) = delete;
        bool finish() noexcept;

      private:
        windows_emulator& owner_;
        bool finished_{};
        bool complete_{true};
    };
}
