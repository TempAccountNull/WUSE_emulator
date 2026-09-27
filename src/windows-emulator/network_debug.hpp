#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace sogen
{
    class windows_emulator;
    struct io_device_context;
    struct syscall_context;
    struct io_completion_message;

    // Optional, bounded JSONL sidecar. No guest state or network behavior depends on it.
    class network_debug_logger
    {
      public:
        network_debug_logger();
        ~network_debug_logger();
        network_debug_logger(const network_debug_logger&) = delete;
        network_debug_logger& operator=(const network_debug_logger&) = delete;

        void open(const std::filesystem::path& path);
        bool enabled() const noexcept;

        void begin_afd_request(windows_emulator& win_emu, io_device_context& request, const syscall_context& issuer,
                               std::string_view syscall_name) noexcept;
        void afd_result(windows_emulator& win_emu, const io_device_context& request, int32_t status) noexcept;
        void afd_completion(windows_emulator& win_emu, const io_device_context& request, int32_t status, bool was_pending) noexcept;
        void apc_queue(uint64_t request_id, uint32_t thread_id, int32_t status, uint64_t information) noexcept;
        void apc_dispatch(uint64_t request_id, uint32_t thread_id) noexcept;
        void iocp_queue(uint64_t request_id, uint64_t port, uint64_t key, int32_t status, uint64_t information) noexcept;
        void iocp_dequeue(const io_completion_message& message, uint64_t port) noexcept;

      private:
        struct implementation;
        std::unique_ptr<implementation> impl_;
    };
}
