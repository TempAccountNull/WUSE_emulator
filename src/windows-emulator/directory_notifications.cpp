#include "std_include.hpp"
#include "directory_notifications.hpp"
#include "io_completion_wait.hpp"
#include "windows_emulator.hpp"

#ifdef OS_WINDOWS
#include <utils/nt_handle.hpp>
#endif

namespace sogen
{
    class native_directory_watch
    {
      public:
#ifdef OS_WINDOWS
        utils::nt::handle<utils::nt::invalid_handle> directory{};
#endif

        explicit native_directory_watch(const std::filesystem::path& path)
        {
#ifdef OS_WINDOWS
            this->directory = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (!this->directory)
            {
                throw std::runtime_error("Cannot open directory notification target");
            }
#else
            (void)path;
            throw std::runtime_error("Native directory notifications require a Windows host");
#endif
        }
    };

    class native_directory_request
    {
      public:
        NTSTATUS status{STATUS_PENDING};
        std::vector<std::byte> bytes{};

#ifdef OS_WINDOWS
        struct native_io_status
        {
            uintptr_t status{};
            uintptr_t information{};
        };

        using notify_function = LONG(NTAPI*)(HANDLE, HANDLE, void*, void*, native_io_status*, void*, ULONG, ULONG, unsigned char);
        using cancel_function = LONG(NTAPI*)(HANDLE, native_io_status*, native_io_status*);
        std::shared_ptr<native_directory_watch> watch{};
        utils::nt::handle<> event{};
        native_io_status iosb{};

        static notify_function notify_api()
        {
            static const auto function =
                reinterpret_cast<notify_function>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtNotifyChangeDirectoryFile"));
            return function;
        }

        static cancel_function cancel_api()
        {
            static const auto function =
                reinterpret_cast<cancel_function>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCancelIoFileEx"));
            return function;
        }
#endif

        native_directory_request(std::shared_ptr<native_directory_watch> watch, uint32_t length, uint32_t filter, bool watch_tree)
        {
#ifdef OS_WINDOWS
            this->watch = std::move(watch);
            this->bytes.resize(length);
            this->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!this->event || !notify_api() || !cancel_api())
            {
                this->status = STATUS_NOT_SUPPORTED;
                return;
            }
            this->status = static_cast<NTSTATUS>(notify_api()(this->watch->directory, this->event, nullptr, nullptr, &this->iosb,
                                                              length ? this->bytes.data() : nullptr, length, filter, watch_tree));
#else
            (void)watch;
            (void)length;
            (void)filter;
            (void)watch_tree;
            this->status = STATUS_NOT_SUPPORTED;
#endif
        }

        ~native_directory_request()
        {
            this->cancel();
        }

        void cancel()
        {
#ifdef OS_WINDOWS
            if (this->status == STATUS_PENDING && this->event && cancel_api())
            {
                native_io_status cancel_status{};
                (void)cancel_api()(this->watch->directory, &this->iosb, &cancel_status);
                (void)WaitForSingleObject(this->event, INFINITE);
                this->status = static_cast<NTSTATUS>(this->iosb.status);
            }
#endif
        }

        std::pair<NTSTATUS, std::vector<std::byte>> observe() const
        {
#ifdef OS_WINDOWS
            if (this->status == STATUS_PENDING && WaitForSingleObject(this->event, 0) != WAIT_OBJECT_0)
            {
                return {STATUS_PENDING, {}};
            }
            if (this->status != STATUS_PENDING && this->status != STATUS_SUCCESS)
            {
                return {this->status, {}};
            }
            const auto size = std::min(this->iosb.information, this->bytes.size());
            return {static_cast<NTSTATUS>(this->iosb.status),
                    {this->bytes.begin(), this->bytes.begin() + static_cast<std::ptrdiff_t>(size)}};
#else
            return {STATUS_NOT_SUPPORTED, {}};
#endif
        }
    };

    void directory_notification_request::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->event);
        buffer.write(this->thread_id);
        buffer.write(this->apc_routine);
        buffer.write(this->apc_context);
        buffer.write(this->io_status_block);
        buffer.write(this->output_buffer);
        buffer.write(this->length);
        const auto observed = this->native ? this->native->observe() : std::pair{this->status, this->result};
        buffer.write(observed.first);
        buffer.write_vector(observed.second);
    }

    void directory_notification_request::deserialize(utils::buffer_deserializer& buffer)
    {
        this->native.reset();
        buffer.read(this->event);
        buffer.read(this->thread_id);
        buffer.read(this->apc_routine);
        buffer.read(this->apc_context);
        buffer.read(this->io_status_block);
        buffer.read(this->output_buffer);
        buffer.read(this->length);
        buffer.read(this->status);
        buffer.read_vector(this->result);
    }

    void directory_notification_state::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->path.u16string());
        buffer.write(this->filter);
        buffer.write(this->watch_tree);
        buffer.write(this->signaled);
        buffer.write(static_cast<uint64_t>(this->requests.size()));
        for (const auto& request : this->requests)
        {
            buffer.write(request);
        }
    }

    void directory_notification_state::deserialize(utils::buffer_deserializer& buffer)
    {
        this->requests.clear();
        this->native.reset();
        this->path = buffer.read<std::u16string>();
        buffer.read(this->filter);
        buffer.read(this->watch_tree);
        buffer.read(this->signaled);
        const auto count = buffer.read<uint64_t>();
        for (uint64_t i = 0; i < count; ++i)
        {
            buffer.read(this->requests.emplace_back());
        }
    }

    namespace
    {
        bool is_writable_buffer(memory_manager& memory, uint64_t address, uint64_t size)
        {
            if (size > UINT64_MAX - address)
            {
                return false;
            }
            const auto end = address + size;
            while (address < end)
            {
                const auto region = memory.get_region_info(address);
                if (!region.is_committed || (region.permissions.common & memory_permission::write) == memory_permission::none ||
                    region.permissions.extended != memory_permission_ext::none)
                {
                    return false;
                }
                const auto next = region.start + region.length;
                if (next <= address)
                {
                    return false;
                }
                address = std::min(next, end);
            }
            return true;
        }

        NTSTATUS submit_request(directory_notification_state& state, directory_notification_request& request)
        {
            try
            {
                if (!state.native)
                {
                    state.native = std::make_shared<native_directory_watch>(state.path);
                }
                request.native = std::make_shared<native_directory_request>(state.native, request.length, state.filter, state.watch_tree);
                return request.native->status;
            }
            catch (const std::bad_alloc&)
            {
                return STATUS_NO_MEMORY;
            }
            catch (const std::runtime_error&)
            {
                return STATUS_NOT_SUPPORTED;
            }
        }

        void complete_request(windows_emulator& win_emu, directory_notification_request& request, NTSTATUS status,
                              const std::vector<std::byte>& bytes)
        {
            if (!bytes.empty())
            {
                win_emu.memory.write_memory(request.output_buffer, bytes.data(), bytes.size());
            }
            if (win_emu.process.is_wow64_process)
            {
                const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu32>>> iosb{win_emu.memory, request.io_status_block};
                iosb.write({.Status = status, .Information = static_cast<uint32_t>(bytes.size())});
            }
            else
            {
                const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> iosb{win_emu.memory, request.io_status_block};
                iosb.write({.Status = status, .Information = bytes.size()});
            }
            if (auto* event = win_emu.process.events.get(request.event))
            {
                event->signaled = true;
            }
            io_completion_wait::release_handle_reference(win_emu.process, request.event);
            const auto thread = win_emu.process.thread_handles_by_id.find(request.thread_id);
            if (request.apc_routine && thread != win_emu.process.thread_handles_by_id.end())
            {
                if (auto* target = win_emu.process.threads.get(thread->second))
                {
                    target->pending_apcs.push_back({.flags = 0,
                                                    .apc_routine = request.apc_routine,
                                                    .apc_argument1 = request.apc_context,
                                                    .apc_argument2 = request.io_status_block,
                                                    .apc_argument3 = 0});
                }
            }
        }
    }

    NTSTATUS directory_notification_manager::begin(windows_emulator& win_emu, const handle file_handle,
                                                   directory_notification_request request, uint32_t filter, bool watch_tree)
    {
        auto& process = win_emu.process;
        const auto* target = process.files.get(file_handle);
        if (!target)
        {
            return STATUS_INVALID_HANDLE;
        }
        if (!target->is_directory())
        {
            return STATUS_INVALID_PARAMETER;
        }
        if ((target->access_mask & (FILE_LIST_DIRECTORY | GENERIC_READ | GENERIC_ALL)) == 0)
        {
            return STATUS_ACCESS_DENIED;
        }
        const auto status_size = process.is_wow64_process ? 8u : 16u;
        if (!request.io_status_block || (request.length && !request.output_buffer) ||
            !is_writable_buffer(win_emu.memory, request.io_status_block, status_size) ||
            !is_writable_buffer(win_emu.memory, request.output_buffer, request.length))
        {
            return STATUS_ACCESS_VIOLATION;
        }
        if (request.event.bits && !process.events.get(request.event))
        {
            return STATUS_INVALID_HANDLE;
        }
        auto [it, inserted] = this->watches_.try_emplace(file_handle.bits);
        auto& state = it->second;
        if (inserted)
        {
            state.path = target->host_path;
            state.filter = filter;
            state.watch_tree = watch_tree;
        }
        const auto status = submit_request(state, request);
        win_emu.log.log("Directory notification: handle=0x%llx path=%s filter=0x%x tree=%u bytes=%u status=0x%x\n",
                        static_cast<unsigned long long>(file_handle.bits), u16_to_u8(target->name).c_str(), state.filter,
                        static_cast<unsigned>(state.watch_tree), request.length, status);
        if (status != STATUS_PENDING && status != STATUS_SUCCESS)
        {
            if (inserted)
            {
                this->watches_.erase(it);
            }
            return status;
        }
        if (request.event.bits)
        {
            request.event = *process.events.duplicate(request.event);
            process.events.get(request.event)->signaled = false;
        }
        state.signaled = false;
        state.requests.push_back(std::move(request));
        this->process_completions(win_emu);
        return status;
    }

    void directory_notification_manager::process_completions(windows_emulator& win_emu)
    {
        for (auto watch = this->watches_.begin(); watch != this->watches_.end();)
        {
            auto& state = watch->second;
            const bool closed = !win_emu.process.files.get(watch->first);
            for (auto it = state.requests.begin(); it != state.requests.end();)
            {
                if (closed)
                {
                    if (it->native)
                    {
                        it->native->cancel();
                    }
                    else if (it->status == STATUS_PENDING)
                    {
                        it->status = STATUS_CANCELLED;
                    }
                }
                else if (it->status == STATUS_PENDING && !it->native)
                {
                    it->status = submit_request(state, *it);
                }
                const auto observed = it->native ? it->native->observe() : std::pair{it->status, it->result};
                if (observed.first == STATUS_PENDING)
                {
                    ++it;
                    continue;
                }
                complete_request(win_emu, *it, observed.first, observed.second);
                state.signaled = true;
                it = state.requests.erase(it);
            }
            if (closed)
            {
                watch = this->watches_.erase(watch);
            }
            else
            {
                ++watch;
            }
        }
    }

    NTSTATUS directory_notification_manager::cancel(windows_emulator& win_emu, const handle file_handle, const uint64_t io_status_block,
                                                    const uint32_t thread_id)
    {
        const auto watch = this->watches_.find(file_handle.bits);
        if (watch == this->watches_.end())
        {
            return STATUS_NOT_FOUND;
        }
        bool cancelled = false;
        for (auto& request : watch->second.requests)
        {
            if ((io_status_block && request.io_status_block != io_status_block) || (thread_id && request.thread_id != thread_id))
            {
                continue;
            }
            if (request.native)
            {
                request.native->cancel();
            }
            else
            {
                request.status = STATUS_CANCELLED;
            }
            cancelled = true;
        }
        this->process_completions(win_emu);
        return cancelled ? STATUS_SUCCESS : STATUS_NOT_FOUND;
    }

    void directory_notification_manager::close(windows_emulator& win_emu, const handle file_handle)
    {
        (void)this->cancel(win_emu, file_handle);
        this->watches_.erase(file_handle.bits);
    }

    bool directory_notification_manager::is_signaled(const handle file_handle) const
    {
        const auto it = this->watches_.find(file_handle.bits);
        return it == this->watches_.end() || it->second.signaled;
    }

    void directory_notification_manager::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write_map(this->watches_);
    }

    void directory_notification_manager::deserialize(utils::buffer_deserializer& buffer)
    {
        this->watches_.clear();
        buffer.read_map(this->watches_);
    }
}
