#include "std_include.hpp"
#include "syscall_dispatcher.hpp"
#include "syscall_utils.hpp"

#include <utils/string.hpp>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace
{
#ifdef _WIN32
    struct syscall_native_av_capture
    {
        // 0: empty, 1: writing, 2: complete. The VEH copies only fixed-size fields.
        std::atomic<uint32_t> state{0};
        std::atomic<uint32_t> phase{0}; // 1: on_syscall, 2: handler, 3: completion callback
        uint32_t syscall_id{};
        uint32_t guest_tid{};
        uint32_t vcpu_index{};
        uint32_t host_tid{};
        uint64_t guest_rip{};
        HANDLE stderr_handle{};
        uint32_t fault_phase{};
        uint64_t native_rip{};
        uint64_t native_rsp{};
        uint64_t native_rbp{};
        uint64_t exception_address{};
        uint64_t fault_address{};
        uint64_t access_type{};
        uint32_t exception_code{};
    };

    thread_local syscall_native_av_capture* active_syscall_av_capture{};
    std::atomic<uint32_t> syscall_native_av_packets{};

    struct syscall_native_av_line
    {
        char data[384]{};
        uint32_t length{};

        void append(const char* text) noexcept
        {
            while (*text && this->length < sizeof(this->data) - 1)
            {
                this->data[this->length++] = *text++;
            }
        }

        void decimal(uint64_t value) noexcept
        {
            char digits[20]{};
            uint32_t count = 0;
            do
            {
                digits[count++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value && count < sizeof(digits));
            while (count)
            {
                const char digit = digits[--count];
                if (this->length < sizeof(this->data) - 1)
                {
                    this->data[this->length++] = digit;
                }
            }
        }

        void hex(uint64_t value) noexcept
        {
            constexpr char digits[] = "0123456789ABCDEF";
            this->append("0x");
            bool started = false;
            for (int shift = 60; shift >= 0; shift -= 4)
            {
                const auto nibble = static_cast<uint32_t>((value >> shift) & 0xF);
                if (nibble || started || shift == 0)
                {
                    started = true;
                    if (this->length < sizeof(this->data) - 1)
                    {
                        this->data[this->length++] = digits[nibble];
                    }
                }
            }
        }
    };

    void write_syscall_native_av_packet(const syscall_native_av_capture& capture) noexcept
    {
        if (!capture.stderr_handle || capture.stderr_handle == INVALID_HANDLE_VALUE)
        {
            return;
        }
        auto count = syscall_native_av_packets.load(std::memory_order_relaxed);
        while (count < 8 && !syscall_native_av_packets.compare_exchange_weak(count, count + 1, std::memory_order_relaxed))
        {
        }
        if (count >= 8)
        {
            return;
        }

        syscall_native_av_line line{};
        line.append("[HOSTAV-FIRST] code=");
        line.hex(capture.exception_code);
        line.append(" host_tid=");
        line.decimal(capture.host_tid);
        line.append(" guest_tid=");
        line.decimal(capture.guest_tid);
        line.append(" vcpu=");
        line.decimal(capture.vcpu_index);
        line.append(" syscall_id=");
        line.hex(capture.syscall_id);
        line.append(" phase=");
        line.decimal(capture.fault_phase);
        line.append(" guest_rip=");
        line.hex(capture.guest_rip);
        line.append(" native_rip=");
        line.hex(capture.native_rip);
        line.append(" fault=");
        line.hex(capture.fault_address);
        line.append(" access=");
        line.hex(capture.access_type);
        line.append(" native_rsp=");
        line.hex(capture.native_rsp);
        line.append("\n");
        DWORD written{};
        WriteFile(capture.stderr_handle, line.data, line.length, &written, nullptr);
    }

    LONG CALLBACK capture_syscall_native_av(EXCEPTION_POINTERS* exception) noexcept
    {
        if (!exception || !exception->ExceptionRecord || !exception->ContextRecord ||
            exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        auto* capture = active_syscall_av_capture;
        if (!capture)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        uint32_t empty = 0;
        if (!capture->state.compare_exchange_strong(empty, 1, std::memory_order_relaxed))
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const auto& record = *exception->ExceptionRecord;
        const auto& context = *exception->ContextRecord;
        capture->exception_code = record.ExceptionCode;
        capture->fault_phase = capture->phase.load(std::memory_order_relaxed);
        capture->exception_address = reinterpret_cast<uint64_t>(record.ExceptionAddress);
        capture->fault_address = record.NumberParameters >= 2 ? record.ExceptionInformation[1] : 0;
        capture->access_type = record.NumberParameters >= 1 ? record.ExceptionInformation[0] : 0;
#if defined(_M_X64) || defined(__x86_64__)
        capture->native_rip = context.Rip;
        capture->native_rsp = context.Rsp;
        capture->native_rbp = context.Rbp;
#else
        capture->native_rip = context.Eip;
        capture->native_rsp = context.Esp;
        capture->native_rbp = context.Ebp;
#endif
        write_syscall_native_av_packet(*capture);
        capture->state.store(2, std::memory_order_release);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    bool native_av_capture_enabled()
    {
        static const bool enabled = [] {
            const char* requested = std::getenv("SOGEN_SYSCALL_NATIVE_AV_PACKET");
            return requested && requested[0] == '1' && requested[1] == '\0' &&
                   AddVectoredExceptionHandler(1, capture_syscall_native_av) != nullptr;
        }();
        return enabled;
    }

    class syscall_native_av_scope
    {
      public:
        explicit syscall_native_av_scope(syscall_native_av_capture& capture) noexcept
            : previous_(active_syscall_av_capture)
        {
            active_syscall_av_capture = &capture;
        }

        ~syscall_native_av_scope()
        {
            active_syscall_av_capture = this->previous_;
        }

        syscall_native_av_scope(const syscall_native_av_scope&) = delete;
        syscall_native_av_scope& operator=(const syscall_native_av_scope&) = delete;

      private:
        syscall_native_av_capture* previous_{};
    };
#endif
}

namespace sogen
{

    static void serialize(utils::buffer_serializer& buffer, const syscall_handler_entry& obj)
    {
        buffer.write(obj.name);
    }

    static void deserialize(utils::buffer_deserializer& buffer, syscall_handler_entry& obj)
    {
        buffer.read(obj.name);
        obj.handler = nullptr;
    }

    void syscall_dispatcher::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write_map(this->handlers_);
    }

    void syscall_dispatcher::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read_map(this->handlers_);
        this->add_handlers();
        this->add_callbacks();
    }

    void syscall_dispatcher::setup(const exported_symbols& ntdll_exports, const std::span<const std::byte> ntdll_data,
                                   const exported_symbols& win32u_exports, const std::span<const std::byte> win32u_data)
    {
        this->handlers_ = {};

        const auto ntdll_syscalls = find_syscalls(ntdll_exports, ntdll_data);
        const auto win32u_syscalls = find_syscalls(win32u_exports, win32u_data);

        map_syscalls(this->handlers_, ntdll_syscalls);
        map_syscalls(this->handlers_, win32u_syscalls);

        this->add_handlers();
        this->add_callbacks();
    }

    void syscall_dispatcher::add_handlers()
    {
        std::map<std::string, syscall_handler> handler_mapping{};
        syscall_dispatcher::add_handlers(handler_mapping);

        for (auto& entry : this->handlers_ | std::views::values)
        {
            const auto handler = handler_mapping.find(entry.name);
            if (handler == handler_mapping.end())
            {
                continue;
            }

            entry.handler = handler->second;

#ifndef NDEBUG
            handler_mapping.erase(handler);
#endif
        }
    }

    void syscall_dispatcher::dispatch(windows_emulator& win_emu, vcpu_context& vcpu)
    {
        auto& emu = vcpu.cpu;
        auto& context = win_emu.process;

        const auto address = emu.read_instruction_pointer();
        const auto raw_syscall_id = emu.reg<uint32_t>(x86_register::eax);
        const auto syscall_id = raw_syscall_id & 0x3FFF; // Only take low bits for WOW64 compatibility, match windoows wraparound

        const auto entry = this->handlers_.find(syscall_id);
        const auto* syscall_name = (entry != this->handlers_.end()) ? entry->second.name.c_str() : "<unknown>";

        const syscall_context c{
            .win_emu = win_emu,
            .emu = emu,
            .vcpu = vcpu,
            .proc = context,
            .write_status = true,
        };

#ifdef _WIN32
        std::optional<syscall_native_av_capture> native_av;
        std::optional<syscall_native_av_scope> native_av_scope;
        if (native_av_capture_enabled())
        {
            native_av.emplace();
            native_av->syscall_id = syscall_id;
            native_av->guest_tid = vcpu.active_thread ? vcpu.active_thread->id : 0;
            native_av->vcpu_index = static_cast<uint32_t>(emu.index());
            native_av->host_tid = GetCurrentThreadId();
            native_av->guest_rip = address;
            native_av->stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
            native_av_scope.emplace(*native_av);
        }
#endif

        try
        {
            if (entry == this->handlers_.end())
            {
                win_emu.log.error("Unknown syscall: 0x%X (raw: 0x%X)\n", syscall_id, raw_syscall_id);
                win_emu.record_stop(stop_reason::unknown_syscall, "0x" + utils::string::to_hex_number(syscall_id));
                c.emu.reg<uint64_t>(x86_register::rax, STATUS_NOT_SUPPORTED);
                win_emu.stop();
                return;
            }

#ifdef _WIN32
            if (native_av) native_av->phase.store(1, std::memory_order_relaxed);
#endif
            const auto res = win_emu.callbacks.on_syscall(syscall_id, entry->second.name);
            if (res == instruction_hook_continuation::skip_instruction)
            {
                return;
            }

            if (!entry->second.handler)
            {
                const auto tid = vcpu.active_thread ? vcpu.active_thread->id : 0;
                win_emu.log.error("Unimplemented syscall: %s - 0x%X (raw: 0x%X) at RIP 0x%llX tid %u vCPU %zu\n",
                                  entry->second.name.c_str(), syscall_id, raw_syscall_id,
                                  static_cast<unsigned long long>(address), tid, emu.index());
                win_emu.record_stop(stop_reason::unimplemented_syscall,
                                    entry->second.name + " at RIP 0x" + utils::string::to_hex_number(address) +
                                        " tid " + std::to_string(tid) + " vCPU " + std::to_string(emu.index()));
                c.emu.reg<uint64_t>(x86_register::rax, STATUS_NOT_SUPPORTED);
                win_emu.stop();
                return;
            }

#ifdef _WIN32
            if (native_av) native_av->phase.store(2, std::memory_order_relaxed);
#endif
            entry->second.handler(c);

#ifdef _WIN32
            if (native_av) native_av->phase.store(3, std::memory_order_relaxed);
#endif
            dispatch_callback(win_emu, entry->second.name);
        }
        catch (std::exception& e)
        {
            const auto* module = win_emu.mod_manager.find_name(address);
            win_emu.log.error("Syscall %s threw an exception: 0x%X (raw: 0x%X) @ Module: %s (0x%" PRIx64 ") - %s\n",
                              syscall_name, syscall_id, raw_syscall_id, module ? module : "<unknown>", address, e.what());
            win_emu.record_stop(stop_reason::syscall_exception, std::string(syscall_name) + ": " + e.what());
            emu.reg<uint64_t>(x86_register::rax, STATUS_UNSUCCESSFUL);
            win_emu.stop();
        }
        catch (...)
        {
            const auto* module = win_emu.mod_manager.find_name(address);
            win_emu.log.error("Syscall %s threw an unknown exception: 0x%X (raw: 0x%X) @ Module: %s (0x%" PRIx64 ")\n",
                              syscall_name, syscall_id, raw_syscall_id, module ? module : "<unknown>", address);
            win_emu.record_stop(stop_reason::syscall_exception, std::string(syscall_name) + ": <unknown exception>");
            emu.reg<uint64_t>(x86_register::rax, STATUS_UNSUCCESSFUL);
            win_emu.stop();
        }
    }

    void syscall_dispatcher::dispatch_callback(windows_emulator& win_emu, std::string& syscall_name)
    {
        // active_cpu(), not emu(): this runs under the syscall's scoped_dispatch, and with more than one
        // vCPU the instrumentation-callback redirect must rewrite the acting vCPU's RIP/r10, not vCPU 0's.
        auto& emu = win_emu.active_cpu();
        auto& context = win_emu.process;

        if (context.instrumentation_callback != 0 && syscall_name != "NtContinue")
        {
            auto rip_old = emu.reg<uint64_t>(x86_register::rip);

            // The increase in RIP caused by executing the syscall here has not yet occurred.
            // If RIP is set directly, it will lead to an incorrect address, so the length of
            // the syscall instruction needs to be subtracted.
            emu.reg<uint64_t>(x86_register::rip, context.instrumentation_callback - 2);

            emu.reg<uint64_t>(x86_register::r10, rip_old);
        }
    }

    dispatch_result syscall_dispatcher::dispatch_completion(windows_emulator& win_emu, vcpu_context& vcpu, callback_id callback_id,
                                                            completion_state* completion_state, const user_callback_result& callback_result)
    {
        auto& emu = vcpu.cpu;

        const syscall_context c{.win_emu = win_emu,
                                .emu = emu,
                                .vcpu = vcpu,
                                .proc = win_emu.process,
                                .write_status = true,
                                .is_callback_completion = true,
                                .current_completion_state = completion_state,
                                .previous_callback_result = callback_result};

        const auto entry = this->completion_handlers_.find(callback_id);

        if (entry == this->completion_handlers_.end())
        {
            win_emu.log.error("Unknown callback: 0x%X\n", static_cast<uint32_t>(callback_id));
            win_emu.stop();
            return dispatch_result::error;
        }

        try
        {
            entry->second(c);
            return c.run_callback ? dispatch_result::new_callback : dispatch_result::completed;
        }
        catch (std::exception& e)
        {
            win_emu.log.error("Completion for callback 0x%X threw an exception - %s\n", static_cast<int>(callback_id), e.what());
            win_emu.stop();
            return dispatch_result::error;
        }
        catch (...)
        {
            win_emu.log.error("Completion for callback 0x%X threw an unknown exception\n", static_cast<int>(callback_id));
            win_emu.stop();
            return dispatch_result::error;
        }
    }

    syscall_dispatcher::syscall_dispatcher(const exported_symbols& ntdll_exports, const std::span<const std::byte> ntdll_data,
                                           const exported_symbols& win32u_exports, const std::span<const std::byte> win32u_data)
    {
        this->setup(ntdll_exports, ntdll_data, win32u_exports, win32u_data);
    }

    std::map<callback_id, std::function<std::unique_ptr<completion_state>()>> syscall_dispatcher::completion_state_factories_{};

} // namespace sogen
