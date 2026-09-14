#pragma once

#include "emulator_utils.hpp"
#include "handles.hpp"
#include <deque>
#include <filesystem>
#include <map>
#include <memory>

namespace sogen
{
    class windows_emulator;
    class native_directory_watch;
    class native_directory_request;

    struct directory_notification_request
    {
        handle event{};
        uint32_t thread_id{};
        uint64_t apc_routine{};
        uint64_t apc_context{};
        uint64_t io_status_block{};
        uint64_t output_buffer{};
        uint32_t length{};
        NTSTATUS status{STATUS_PENDING};
        std::vector<std::byte> result{};
        std::shared_ptr<native_directory_request> native{};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    struct directory_notification_state
    {
        std::filesystem::path path{};
        uint32_t filter{};
        bool watch_tree{};
        bool signaled{true};
        std::deque<directory_notification_request> requests{};
        std::shared_ptr<native_directory_watch> native{};

        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);
    };

    class directory_notification_manager
    {
      public:
        NTSTATUS begin(windows_emulator& win_emu, handle file_handle, directory_notification_request request, uint32_t filter,
                       bool watch_tree);
        NTSTATUS cancel(windows_emulator& win_emu, handle file_handle, uint64_t io_status_block = 0, uint32_t thread_id = 0);
        void close(windows_emulator& win_emu, handle file_handle);
        void process_completions(windows_emulator& win_emu);
        bool is_signaled(handle file_handle) const;
        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer);

      private:
        std::map<uint64_t, directory_notification_state> watches_{};
    };
}
