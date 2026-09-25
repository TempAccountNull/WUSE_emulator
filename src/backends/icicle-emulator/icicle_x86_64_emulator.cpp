#define ICICLE_EMULATOR_IMPL
#include "icicle_x86_64_emulator.hpp"
#include "execution_hook.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <utils/object.hpp>
#include <utils/finally.hpp>

using icicle_emulator = struct icicle_emulator_;

extern "C"
{
    using icicle_mmio_read_func = void(void* user, uint64_t address, void* data, size_t length);
    using icicle_mmio_write_func = void(void* user, uint64_t address, const void* data, size_t length);

    using raw_func = void(void*);
    using instruction_func = uint32_t(void*);
    using ptr_func = void(void*, uint64_t);
    using block_func = void(void*, uint64_t, uint64_t);
    using interrupt_func = void(void*, int32_t);
    using violation_func = int32_t(void*, uint64_t address, uint8_t operation, int32_t unmapped);
    using data_accessor_func = void(void* user, const void* data, size_t length);
    using memory_access_func = icicle_mmio_write_func;
    using write_observation_func = void(void*, uint64_t, const void*, size_t, uint64_t, int32_t);

    struct icicle_stop_info
    {
        uint32_t kind;
        uint32_t code;
        uint64_t value;
    };

    struct icicle_exec_write_wake_profile
    {
        uint64_t wake_events, guest_jit_writes, guest_mmu_writes, host_writes, owner_flushes;
    };

    struct icicle_exec_write_filter_profile
    {
        uint64_t filtered_noncode, notified_overlap, notified_concurrent;
    };
    static_assert(sizeof(icicle_exec_write_filter_profile) == 3 * sizeof(uint64_t));

    struct icicle_invalidation_profile
    {
        uint64_t epoch_mismatches, jit_resets, epoch_resets, wake_resets, manual_resets, mixed_resets, unknown_resets;
    };
    static_assert(sizeof(icicle_invalidation_profile) == 7 * sizeof(uint64_t));

    struct icicle_manual_invalidation_profile
    {
        uint64_t total, peer_protection, public_invalidate, self_modifying, host_cache;
        uint64_t unmap, protect, host_write, multiple_origins, unknown_origin;
    };
    static_assert(sizeof(icicle_manual_invalidation_profile) == 10 * sizeof(uint64_t));

    struct icicle_jit_profile
    {
        uint64_t compile_calls, compile_nanos, reset_calls;
        uint64_t recompile_calls, recompile_nanos;
        uint64_t recompile_compile_calls, recompile_compile_nanos;
        uint64_t reset_generation, reset_cause_flags, reset_manual_origin_flags;
        uint64_t flush_code_nanos, jit_reset_nanos;
        uint64_t generation_compile_calls, generation_compile_nanos;
        uint64_t origin_first_address_compiles, origin_repeat_after_reset_compiles;
        uint64_t origin_repeat_in_generation_compiles, origin_periodic_recompile_compiles;
        uint64_t origin_unclassified_compiles, origin_generation_number;
    };
    static_assert(sizeof(icicle_jit_profile) == 20 * sizeof(uint64_t));

    icicle_emulator* icicle_create_emulator(uint64_t memory_limit_mib);
    int32_t icicle_link_exec_write_wake(icicle_emulator* const* handles, size_t count);
    int32_t icicle_exec_write_pending(icicle_emulator*);
    int32_t icicle_get_exec_write_wake_profile(icicle_emulator*, icicle_exec_write_wake_profile*);
    int32_t icicle_get_exec_write_filter_profile(icicle_emulator*, icicle_exec_write_filter_profile*);
    int32_t icicle_get_invalidation_profile(icicle_emulator*, icicle_invalidation_profile*);
    int32_t icicle_get_manual_invalidation_profile(icicle_emulator*, icicle_manual_invalidation_profile*);
    void icicle_reconcile_exec_write_wake(icicle_emulator*);
    int32_t icicle_protect_memory(icicle_emulator*, uint64_t address, uint64_t length, uint8_t permissions);
    int32_t icicle_map_memory(icicle_emulator*, uint64_t address, uint64_t length, uint8_t permissions);
    int32_t icicle_map_mmio(icicle_emulator*, uint64_t address, uint64_t length, icicle_mmio_read_func* read_callback, void* read_data,
                            icicle_mmio_write_func* write_callback, void* write_data);
    int32_t icicle_map_shared_memory(icicle_emulator*, uint64_t address, uint64_t source, uint64_t length, uint8_t permissions);
    // SMP shared RAM (steps 1-2): map one shared page set on a master VM, then alias the same
    // Arc<PageData> into every other vCPU VM so all N see one coherent, cachable, write-through address space.
    int32_t icicle_map_smp_shared_fresh(icicle_emulator*, uint64_t address, uint64_t length, uint8_t permissions);
    int32_t icicle_share_smp_pages(icicle_emulator* dst, icicle_emulator* src, uint64_t address, uint64_t length);
    // SMP async (step 6.5): capture a range's shared pages from a VM on its own thread; a peer maps them
    // later (icicle_smp_map_captured) with NO cross-thread read of the (possibly executing) source VM.
    void* icicle_smp_capture(icicle_emulator*, uint64_t address, uint64_t length);
    int32_t icicle_smp_map_captured(icicle_emulator*, void* captured, uint64_t address);
    void icicle_smp_release_capture(void* captured);
    int32_t icicle_unmap_memory(icicle_emulator*, uint64_t address, uint64_t length);
    int32_t icicle_read_memory(icicle_emulator*, uint64_t address, void* data, size_t length);
    int32_t icicle_write_memory(icicle_emulator*, uint64_t address, const void* data, size_t length);
    void icicle_save_registers(icicle_emulator*, data_accessor_func* accessor, void* accessor_data);
    void icicle_restore_registers(icicle_emulator*, const void* data, size_t length);
    void icicle_reset_volatile_state(icicle_emulator*);
    uint32_t icicle_create_snapshot(icicle_emulator*);
    int32_t icicle_map_host_memory(icicle_emulator*, uint64_t, void*, uint64_t, uint8_t);
    int32_t icicle_has_host_mappings(icicle_emulator*);
    void icicle_flush_host_memory_cache(icicle_emulator*, const void*, size_t);
    void icicle_restore_snapshot(icicle_emulator*, uint32_t id);
    uint32_t icicle_add_syscall_hook(icicle_emulator*, raw_func* callback, void* data);
    uint32_t icicle_add_timestamp_hook(icicle_emulator*, int32_t serializing, instruction_func* callback, void* data);
    uint32_t icicle_add_interrupt_hook(icicle_emulator*, interrupt_func* callback, void* data);
    uint32_t icicle_add_block_hook(icicle_emulator*, block_func* callback, void* data);
    uint32_t icicle_add_execution_hook(icicle_emulator*, uint64_t address, ptr_func* callback, void* data);
    uint32_t icicle_add_ranged_execution_hook(icicle_emulator*, uint64_t address, uint64_t size, ptr_func* callback, void* data);
    uint32_t icicle_add_generic_execution_hook(icicle_emulator*, ptr_func* callback, void* data);
    uint32_t icicle_add_violation_hook(icicle_emulator*, violation_func* callback, void* data);
    uint32_t icicle_add_read_hook(icicle_emulator*, uint64_t start, uint64_t end, memory_access_func* cb, void* data);
    uint32_t icicle_add_write_hook(icicle_emulator*, uint64_t start, uint64_t end, memory_access_func* cb, void* data);
    uint32_t icicle_add_write_observation_hook(icicle_emulator*, uint64_t, uint64_t, write_observation_func*, void*);
    void icicle_remove_hook(icicle_emulator*, uint32_t id);
    size_t icicle_read_register(icicle_emulator*, int reg, void* data, size_t length);
    size_t icicle_write_register(icicle_emulator*, int reg, const void* data, size_t length);
    void icicle_start(icicle_emulator*, size_t count);
    int32_t icicle_get_stop_info(icicle_emulator*, icicle_stop_info* info);
    // Retired-instruction counter: used to honor start(count) across kick-resumed quanta.
    uint64_t icicle_get_icount(icicle_emulator*);
    int32_t icicle_get_jit_profile(icicle_emulator*, icicle_jit_profile*);
    // SMP 6.6: drop this VM's translations for a range (returns 1 if anything was cached), and a
    // read-only query for whether ANY sharing VM translated the range (shared IN_CODE_CACHE perms).
    int32_t icicle_invalidate_code_range(icicle_emulator*, uint64_t address, uint64_t length);
    int32_t icicle_code_range_is_cached(icicle_emulator*, uint64_t address, uint64_t length);
    int32_t icicle_host_view_code_may_be_cached(icicle_emulator*, uint64_t address, uint64_t length);
    // SMP 6.6c'': smallest perm epoch over a range (0 = not shared/unmapped) for stale-op detection.
    void icicle_refresh_peer_protection(icicle_emulator*, uint64_t address, uint64_t length);
    uint64_t icicle_perm_epoch_of_range(icicle_emulator*, uint64_t address, uint64_t length);
    void icicle_get_exception_name(uint32_t code, data_accessor_func* callback, void* data);
    void icicle_get_vm_exit_description(icicle_emulator*, data_accessor_func* callback, void* data);
    void icicle_stop(icicle_emulator*);
    void icicle_destroy_emulator(icicle_emulator*);
    void icicle_run_on_next_instruction(icicle_emulator*, raw_func* callback, void* data);
}

namespace sogen::icicle
{
    namespace
    {
        void ice(const bool result, const std::string_view error)
        {
            if (!result)
            {
                throw std::runtime_error(std::string(error));
            }
        }

        // Only large failed host writes emit a native caller chain. The Rust bridge logs the
        // corresponding MMU map state; the C++ memory manager owns reservation/commit metadata.
        // Keep this failure-only diagnostic bounded even if a guest repeatedly retries a bad buffer.
        void diagnose_large_write_failure(const uint64_t address, const size_t size)
        {
            if (size < 4096)
            {
                return;
            }
            static std::atomic<unsigned> emitted{0};
            if (emitted.fetch_add(1, std::memory_order_relaxed) >= 8)
            {
                return;
            }
            void* frames[12]{};
            const auto count = CaptureStackBackTrace(1, 12, frames, nullptr);
            std::fprintf(stderr, "[ICWRITE] failed addr=%#llx size=%zu tid=%u callers=",
                         static_cast<unsigned long long>(address), size, static_cast<unsigned>(GetCurrentThreadId()));
            for (USHORT i = 0; i < count; ++i)
            {
                HMODULE module{};
                const auto address_in_module = reinterpret_cast<LPCSTR>(frames[i]);
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       address_in_module, &module))
                {
                    char path[MAX_PATH]{};
                    GetModuleFileNameA(module, path, MAX_PATH);
                    const char* filename = std::strrchr(path, '\\');
                    filename = filename ? filename + 1 : path;
                    const auto offset = reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(module);
                    std::fprintf(stderr, "%s%s+%#llx", i ? " <- " : "", filename,
                                 static_cast<unsigned long long>(offset));
                }
                else
                {
                    std::fprintf(stderr, "%s%p", i ? " <- " : "", frames[i]);
                }
            }
            std::fprintf(stderr, "\n");
        }

        constexpr size_t context_record_bytes = 1232;

        struct context_write_retry_probe
        {
            uint64_t address{};
            bool initial_failed{};
        };

        void diagnose_context_write_retry(const uint64_t address, const int64_t caller_vcpu, const bool succeeded, const char* disposition)
        {
            static std::atomic<unsigned> emitted{0};
            if (emitted.fetch_add(1, std::memory_order_relaxed) >= 16)
            {
                return;
            }
            std::fprintf(stderr, "[ICWRITE] context-retry addr=%#llx bytes=%zu caller_vcpu=%lld final=%s disposition=%s tid=%u\n",
                         static_cast<unsigned long long>(address), context_record_bytes, static_cast<long long>(caller_vcpu),
                         succeeded ? "success" : "failure", disposition, static_cast<unsigned>(GetCurrentThreadId()));
        }

        // SMP deadlock tracing (SOGEN_SMP_TRACE=1): prints every cross-VM coordination event with
        // values so a hung run's stderr tail shows exactly which wait is stuck and on which vCPU.
        bool smp_trace_enabled()
        {
            static const bool enabled = [] {
                const char* v = std::getenv("SOGEN_SMP_TRACE");
                return v && *v && *v != '0';
            }();
            return enabled;
        }

        using detail::hook_scope;

        template <typename T>
        struct function_object : utils::object
        {
            std::function<T> func{};

            explicit function_object(std::function<T> f = {})
                : func(std::move(f))
            {
            }

            template <typename... Args>
            auto operator()(Args&&... args) const
            {
                // Scope the INVOKING thread's in-hook flag (thread-local under SMP).
                const hook_scope scope;

                return this->func.operator()(std::forward<Args>(args)...);
            }

            ~function_object() override = default;
        };

        template <typename T>
        std::unique_ptr<function_object<T>> make_function_object(std::function<T> func)
        {
            return std::make_unique<function_object<T>>(std::move(func));
        }

        // Stored UNBOUND (still takes cpu_interface&): with N vCPU VMs the hook is registered once per
        // VM and bound to that VM's vCPU at install time (see hook_memory_access), so one hook fires with
        // the correct acting vCPU on whichever VM executed the access.
        struct memory_access_hook
        {
            uint64_t address{};
            uint64_t size{};
            memory_access_hook_callback callback{};
            bool is_read{};
            memory_write_observation_callback observation{};
        };

        // The icicle_vcpu running on the current OS thread (nullptr on non-vCPU threads). Lets a
        // stop-the-world initiated from inside a vCPU's own hook skip pausing/awaiting itself (6.3/6.5).
        thread_local void* t_running_vcpu = nullptr;

    // Legacy start()-scope owner used by in-run fault retries. It must be restored when
    // start() returns: scheduler ownership between quanta is tracked separately below.
    thread_local void* t_worker_vcpu = nullptr;
    // Direct start() callers are not scheduler workers; only the scheduler sets this marker.
    thread_local void* t_scheduler_worker_vcpu = nullptr;
    // The scheduler holds this VM's parked-owner gate only while it has the kernel lock.
    thread_local void* t_parked_vm_vcpu = nullptr;
    // True while THIS thread is inside drain_pending_ops: a drained op (e.g. a map) can re-enter
    // try_write_memory, which would otherwise drain again (re-entrancy broke CrossVmMap/RangedExec).
    thread_local bool t_draining_own_queue = false;
    thread_local context_write_retry_probe* t_context_write_retry_probe = nullptr;

        uint64_t configured_memory_limit_mib()
        {
            const auto* text = std::getenv("SOGEN_ICICLE_MEMORY_MB");
            if (!text)
            {
                return 0;
            }
            const auto* end = text + std::strlen(text);
            uint64_t limit{};
            const auto parsed = std::from_chars(text, end, limit);
            constexpr uint64_t maximum = std::numeric_limits<uint32_t>::max() / 256;
            if (parsed.ec != std::errc{} || parsed.ptr != end || limit == 0 || limit > maximum)
            {
                throw std::runtime_error("SOGEN_ICICLE_MEMORY_MB must be a decimal guest backing limit in MiB (1..16777215)");
            }
            return limit;
        }

        enum class icicle_stop_kind : uint32_t
        {
            none = 0,
            instruction_limit = 1,
            unhandled_exception = 2,
            other = 3,
        };
    }

    class icicle_x86_64_emulator;

    // Separate worker-owned counters keep status reads lock-free. Timing is opt-in and
    // applies only to coarse cross-VM operations, never to guest stores or instructions.
    struct smp_profile_counters
    {
        std::atomic<uint64_t> map_calls{0};
        std::atomic<uint64_t> map_nanos{0};
        std::atomic<uint64_t> peer_map_calls{0};
        std::atomic<uint64_t> peer_map_pages{0};
        std::atomic<uint64_t> peer_map_nanos{0};
        std::atomic<uint64_t> peer_map_max_nanos{0};
        std::atomic<uint64_t> protect_calls{0};
        std::atomic<uint64_t> protect_nanos{0};
        std::atomic<uint64_t> queue_ops{0};
        std::atomic<uint64_t> queue_nanos{0};
        std::atomic<uint64_t> invalidate_queued{0};
        std::atomic<uint64_t> invalidate_queue_nanos{0};
        std::atomic<uint64_t> invalidate_applied{0};
        std::atomic<uint64_t> invalidate_no_change{0};
        std::atomic<uint64_t> invalidate_apply_nanos{0};
        std::atomic<uint64_t> invalidate_adjacent_same_pages{0};
        std::atomic<uint64_t> kick_calls{0};
        std::atomic<uint64_t> kick_targets{0};
        std::atomic<uint64_t> kick_nanos{0};
    };

    struct smp_profile_timer
    {
        explicit smp_profile_timer(std::atomic<uint64_t>* total) : total_(total)
        {
            if (this->total_)
            {
                this->start_ = std::chrono::steady_clock::now();
            }
        }

        ~smp_profile_timer()
        {
            if (this->total_)
            {
                const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - this->start_).count();
                this->total_->fetch_add(static_cast<uint64_t>(nanos), std::memory_order_relaxed);
            }
        }

      private:
        std::atomic<uint64_t>* total_{};
        std::chrono::steady_clock::time_point start_{};
    };

    // One vCPU: register + run state on its own icicle VM handle; memory delegates to the shared
    // machine. Mirrors whp_vcpu so windows-emulator's N-vCPU scheduler can drive N icicle VMs (SMP).
    // The machine (icicle_x86_64_emulator) delegates its own CPU-0 role to vcpus_[0] (see get_cpu).
    class icicle_vcpu final : public x86_64_cpu, public detail::hook_exception_sink
    {
      public:
        icicle_vcpu(icicle_emulator* emu, icicle_x86_64_emulator& machine, const uint32_t index,
                    const bool host_view_only = false)
            : emu_(emu),
              machine_(machine),
              index_(index),
              host_view_only_(host_view_only)
        {
        }

        size_t index() const override
        {
            return this->index_;
        }

        bool has_guest_cpu_context() const override
        {
            return !this->host_view_only_;
        }

        memory_interface& memory() override;
        const memory_interface& memory() const override;

        void start(size_t count) override;

        void stop() override
        {
            if (this->host_view_only_)
            {
                throw std::logic_error("Icicle host-view callback has no running CPU context");
            }
            this->stop_requested_ = true;
            // An idle worker may consume switch_thread before its next start().
            // Leave no stale atomic interrupt that would end that new quantum
            // without a pending scheduler switch. A running quantum still needs
            // the remote interrupt to reach its next VM timer boundary.
            if (this->run_active_.load(std::memory_order_acquire))
            {
                icicle_stop(this->emu_);
            }
        }

        void acknowledge_stop() override
        {
            this->stop_requested_.store(false, std::memory_order_release);
        }

        void load_gdt(const pointer_type address, const uint32_t limit) override
        {
            struct gdtr
            {
                uint32_t padding{};
                uint32_t limit{};
                uint64_t address{};
            };

            const gdtr entry{.limit = limit, .address = address};
            static_assert(sizeof(gdtr) - offsetof(gdtr, limit) == 12);

            this->write_register(x86_register::gdtr, &entry.limit, 12);
        }

        void set_segment_base(const x86_register base, const pointer_type value) override
        {
            switch (base)
            {
            case x86_register::fs:
            case x86_register::fs_base:
                this->reg(x86_register::fs_base, value);
                break;
            case x86_register::gs:
            case x86_register::gs_base:
                this->reg(x86_register::gs_base, value);
                break;
            default:
                break;
            }
        }

        pointer_type get_segment_base(const x86_register base) override
        {
            switch (base)
            {
            case x86_register::fs:
            case x86_register::fs_base:
                return this->reg(x86_register::fs_base);
            case x86_register::gs:
            case x86_register::gs_base:
                return this->reg(x86_register::gs_base);
            default:
                return 0;
            }
        }

        size_t write_raw_register(const int reg, const void* value, const size_t size) override
        {
            if (this->host_view_only_)
            {
                throw std::logic_error("Icicle host-view callback has no register context");
            }
            return icicle_write_register(this->emu_, reg, value, size);
        }

        size_t read_raw_register(const int reg, void* value, const size_t size) override
        {
            if (this->host_view_only_)
            {
                throw std::logic_error("Icicle host-view callback has no register context");
            }
            return icicle_read_register(this->emu_, reg, value, size);
        }

        bool read_descriptor_table(const int reg, descriptor_table_register& table) override
        {
            if (reg != static_cast<int>(x86_register::gdtr) && reg != static_cast<int>(x86_register::idtr))
            {
                return false;
            }

            struct gdtr
            {
                uint32_t padding{};
                uint32_t limit{};
                uint64_t address{};
            };

            gdtr entry{};
            static_assert(sizeof(gdtr) - offsetof(gdtr, limit) == 12);
            if (this->read_raw_register(reg, &entry.limit, 12) != 12)
            {
                return false;
            }

            table.base = entry.address;
            table.limit = entry.limit;
            return true;
        }

        std::vector<std::byte> save_registers() const override
        {
            if (this->host_view_only_)
            {
                throw std::logic_error("Icicle host-view callback has no register context");
            }
            std::vector<std::byte> data{};
            auto* accessor = +[](void* user, const void* data, const size_t length) {
                auto& vec = *static_cast<std::vector<std::byte>*>(user);
                vec.resize(length);
                memcpy(vec.data(), data, length);
            };

            icicle_save_registers(this->emu_, accessor, &data);

            return data;
        }

        void restore_registers(const std::vector<std::byte>& register_data) override
        {
            if (this->host_view_only_)
            {
                throw std::logic_error("Icicle host-view callback has no register context");
            }
            icicle_restore_registers(this->emu_, register_data.data(), register_data.size());
        }

        bool has_violation() const override
        {
            return false;
        }

        bool supports_instruction_counting() const override
        {
            return true;
        }

        bool is_stop_thread_safe() const override
        {
            return true;
        }

        icicle_emulator* handle() const
        {
            return this->emu_;
        }

        // A host exception raised in a hook bound to this vCPU stops THIS VM and is rethrown from this
        // vCPU's start() once icicle has returned to C++ (see execution_hook / hook_exception_sink).
        void defer_hook_exception(std::exception_ptr exception) noexcept override
        {
            if (!this->pending_hook_exception_)
            {
                this->pending_hook_exception_ = std::move(exception);
            }
            icicle_stop(this->emu_);
        }

        void rethrow_deferred_hook_exception()
        {
            if (this->pending_hook_exception_)
            {
                auto exception = std::exchange(this->pending_hook_exception_, std::exception_ptr{});
                std::rethrow_exception(exception);
            }
        }

      private:
        friend class icicle_x86_64_emulator; // reads run_active_/stop_requested_/pending_hook_exception_/index_

        void throw_if_unhandled_stop();

        icicle_emulator* emu_{};
        icicle_x86_64_emulator& machine_;
        uint32_t index_{0};
        bool host_view_only_{false};
        std::exception_ptr pending_hook_exception_{};

        // Step 6.2 (mirrors WHP whp_vcpu::run_active_/stop_requested_): run_active_ is true only while this
        // vCPU is inside icicle run() (a peer's stop-the-world waits for every peer's run_active_ to clear
        // before touching its VM). stop_requested_ distinguishes a REAL stop (thread-switch/external) from a
        // quiesce cancel (a peer's cross-VM mutation), so start() knows whether to return or re-enter.
        std::atomic_bool run_active_{false};
        std::atomic_bool stop_requested_{false};
        // Excludes external stop-the-world VM access from parked scheduler host work.
        std::recursive_mutex parked_vm_mutex_{};

        // Activity telemetry (progress meter): retired-instruction count and last-parked RIP,
        // published by the OWNING worker thread at its between-quanta safe point (VM parked there,
        // so the reads that fill them are race-free). Readers poll vcpu_activity() without
        // stopping peers.
        std::atomic_uint64_t published_instructions_{0};
        std::atomic_uint64_t published_rip_{0};
        smp_profile_counters smp_profile_{};
        std::atomic_uint64_t jit_compile_calls_{0}, jit_compile_nanos_{0}, jit_reset_calls_{0};
        std::atomic_uint64_t jit_recompile_calls_{0}, jit_recompile_nanos_{0};
        std::atomic_uint64_t jit_recompile_compile_calls_{0}, jit_recompile_compile_nanos_{0};
        std::atomic_uint64_t jit_reset_generation_{0}, jit_reset_cause_flags_{0}, jit_reset_manual_origin_flags_{0};
        std::atomic_uint64_t jit_flush_code_nanos_{0}, jit_reset_nanos_{0};
        std::atomic_uint64_t jit_generation_compile_calls_{0}, jit_generation_compile_nanos_{0};
        std::atomic_uint64_t jit_origin_first_address_compiles_{0}, jit_origin_repeat_after_reset_compiles_{0};
        std::atomic_uint64_t jit_origin_repeat_in_generation_compiles_{0}, jit_origin_periodic_recompile_compiles_{0};
        std::atomic_uint64_t jit_origin_unclassified_compiles_{0}, jit_origin_generation_number_{0};
    };

    class icicle_x86_64_emulator : public x86_64_emulator, public detail::hook_exception_sink
    {
      public:
        explicit icicle_x86_64_emulator(size_t vcpu_count = 1)
        {
            if (vcpu_count < 1)
            {
                vcpu_count = 1;
            }
            if (vcpu_count == 1 && this->force_smp_memory_)
            {
                std::fprintf(stderr, "[SMPMEMPROBE] vcpu=0 mode=shared\n");
            }
            // One icicle VM per vCPU (each keeps its single-threaded Rc<RefCell> core pinned to its own
            // OS thread); guest RAM is shared across them via smp_shared pages in map_memory. VM 0 is the
            // master the machine surface (memory read/write) delegates to.
            //
            // INVARIANT (N>1): the machine surface below mutates ALL N VMs (map/unmap/protect touch every
            // handle; each hook_* registers on every handle). An icicle VM must only be touched by its own
            // thread, so these machine-wide mutations are safe only while the VMs are quiesced -- during
            // setup (before workers start) or a stop-the-world kernel section under Sogen's BEL. Coordinating
            // cross-VM mutation that happens *while other vCPUs execute in parallel* is step 6 (see
            // cpu-test-impl/SMP-PLAN.md): defer each VM's mutation to its own thread, or gate on the BEL.
            this->vcpus_.reserve(vcpu_count);
            for (size_t i = 0; i < vcpu_count; ++i)
            {
                auto* handle = icicle_create_emulator(configured_memory_limit_mib());
                if (!handle)
                {
                    throw std::runtime_error("Failed to create icicle emulator instance");
                }
                this->vcpus_.push_back(std::make_unique<icicle_vcpu>(handle, *this, static_cast<uint32_t>(i)));
            }
            this->emu_ = this->vcpus_[0]->handle();
            if (vcpu_count > 1)
            {
                this->host_view_ = icicle_create_emulator(configured_memory_limit_mib());
                if (!this->host_view_)
                {
                    throw std::runtime_error("Failed to create Icicle host memory view");
                }
                this->host_view_cpu_ = std::make_unique<icicle_vcpu>(this->host_view_, *this, 0, true);
                const char* wake = std::getenv("SOGEN_SMP_EXEC_WRITE_WAKE");
                this->exec_write_wake_enabled_ = wake && std::strcmp(wake, "1") == 0;
                if (this->exec_write_wake_enabled_)
                {
                    std::vector<icicle_emulator*> handles;
                    handles.reserve(vcpu_count);
                    for (const auto& vcpu : this->vcpus_)
                    {
                        handles.push_back(vcpu->handle());
                    }
                    if (!icicle_link_exec_write_wake(handles.data(), handles.size()))
                    {
                        throw std::runtime_error("Failed to link Icicle executable-write wake flags");
                    }
                }
            }
            this->quiesce_cancel_.assign(vcpu_count, 0);
            this->quantum_kick_.assign(vcpu_count, 0);
            // Include the external issuer sentinel. A fixed-size atomic array avoids
            // reallocating while a peer applies a queued protect operation.
            this->issuer_seq_ = std::vector<std::atomic<uint64_t>>(vcpu_count + 1);
            this->pending_ops_.resize(vcpu_count);
        }

        ~icicle_x86_64_emulator() override
        {
            // Free hook/storage objects first (nothing is running), then tear down every VM handle.
            // A hook callback may own a scoped_hook whose destructor calls delete_hook().
            // Move the table out before destroying callbacks so that a reentrant
            // delete cannot mutate the container being torn down.
            utils::reset_object_with_delayed_destruction(this->registrations_);
            reset_object_with_delayed_destruction(this->storage_);
            utils::reset_object_with_delayed_destruction(this->hooks_to_install_);

            for (auto& vcpu : this->vcpus_)
            {
                if (vcpu && vcpu->handle())
                {
                    icicle_destroy_emulator(vcpu->handle());
                }
            }
            if (this->host_view_)
            {
                icicle_destroy_emulator(this->host_view_);
                this->host_view_ = nullptr;
            }
            this->emu_ = nullptr;
        }

        // The machine delegates its own CPU-0 role to vcpus_[0] (mirrors whp_x86_64_emulator); the
        // N-vCPU scheduler drives get_cpu(i) directly.
        void start(const size_t count) override
        {
            this->vcpus_[0]->start(count);
        }

        // See detail::hook_exception_sink. Machine-scoped deferrals (e.g. run_on_next_instruction, which
        // runs on the master VM) are attributed to vCPU 0; per-vCPU hook wrappers defer to their own vCPU.
        void defer_hook_exception(std::exception_ptr exception) noexcept override
        {
            this->vcpus_[0]->defer_hook_exception(std::move(exception));
        }

        void stop() override
        {
            this->vcpus_[0]->stop();
        }

        size_t vcpu_count() const override
        {
            return this->vcpus_.size();
        }

        void set_scheduler_worker_context(const size_t index, const bool active) override
        {
            auto* const worker = this->vcpus_.at(index).get();
            if (active)
            {
                t_scheduler_worker_vcpu = worker;
            }
            else if (t_scheduler_worker_vcpu == worker)
            {
                t_scheduler_worker_vcpu = nullptr;
            }
        }

        void set_scheduler_vm_parked(const size_t index, const bool active) override
        {
            if (this->vcpus_.size() == 1)
            {
                return;
            }
            auto* const worker = this->vcpus_.at(index).get();
            if (active)
            {
                if (t_parked_vm_vcpu == worker)
                {
                    return;
                }
                assert(t_scheduler_worker_vcpu == worker && t_parked_vm_vcpu == nullptr);
                worker->parked_vm_mutex_.lock();
                t_parked_vm_vcpu = worker;
            }
            else if (t_parked_vm_vcpu == worker)
            {
                t_parked_vm_vcpu = nullptr;
                worker->parked_vm_mutex_.unlock();
            }
        }

        bool try_set_scheduler_vm_parked(const size_t index) override
        {
            if (this->vcpus_.size() == 1)
            {
                return true;
            }
            auto* const worker = this->vcpus_.at(index).get();
            if (t_parked_vm_vcpu == worker)
            {
                return true;
            }
            assert(t_scheduler_worker_vcpu == worker && t_parked_vm_vcpu == nullptr);
            if (!worker->parked_vm_mutex_.try_lock())
            {
                return false;
            }
            t_parked_vm_vcpu = worker;
            return true;
        }

        x86_64_cpu& get_cpu(const size_t index) override
        {
            return *this->vcpus_.at(index);
        }

        void load_gdt(const pointer_type address, const uint32_t limit) override
        {
            this->vcpus_[0]->load_gdt(address, limit);
        }

        void set_segment_base(const x86_register base, const pointer_type value) override
        {
            this->vcpus_[0]->set_segment_base(base, value);
        }

        pointer_type get_segment_base(const x86_register base) override
        {
            return this->vcpus_[0]->get_segment_base(base);
        }

        size_t write_raw_register(const int reg, const void* value, const size_t size) override
        {
            return this->vcpus_[0]->write_raw_register(reg, value, size);
        }

        size_t read_raw_register(const int reg, void* value, const size_t size) override
        {
            return this->vcpus_[0]->read_raw_register(reg, value, size);
        }

        bool read_descriptor_table(const int reg, descriptor_table_register& table) override
        {
            return this->vcpus_[0]->read_descriptor_table(reg, table);
        }

        void map_mmio(const uint64_t address, const size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override
        {
            struct mmio_wrapper : utils::object
            {
                uint64_t base{};
                mmio_read_callback read_cb{};
                mmio_write_callback write_cb{};
            };

            auto wrapper = std::make_unique<mmio_wrapper>();
            wrapper->base = address;
            wrapper->read_cb = std::move(read_cb);
            wrapper->write_cb = std::move(write_cb);

            auto* ptr = wrapper.get();
            this->storage_.push_back(std::move(wrapper));

            auto* read_wrapper = +[](void* user, const uint64_t addr, void* data, const size_t length) {
                const auto* w = static_cast<mmio_wrapper*>(user);
                w->read_cb(addr - w->base, data, length);
            };

            auto* write_wrapper = +[](void* user, const uint64_t addr, const void* data, const size_t length) {
                const auto* w = static_cast<mmio_wrapper*>(user);
                w->write_cb(addr + w->base, data, length);
            };

            const auto map_vm = [&](icicle_emulator* h) {
                ice(icicle_map_mmio(h, address, size, read_wrapper, ptr, write_wrapper, ptr), "Failed to map MMIO");
            };
            if (!this->host_view_)
            {
                map_vm(this->emu_);
                return;
            }
            const auto map_view = [&] {
                ice(icicle_map_mmio(this->host_view_, address, size, read_wrapper, ptr, write_wrapper, ptr),
                    "Failed to map MMIO in Icicle host view");
                this->mmio_ranges_.emplace_back(address, size);
            };
            if (auto* self = this->mutation_owner())
            {
                {
                    std::lock_guard lock(this->host_view_mutex_);
                    map_vm(self->handle());
                    map_view();
                }
                const auto seq = this->issuer_next(self->index());
                for (auto& vcpu : this->vcpus_)
                {
                    if (vcpu.get() != self)
                    {
                        this->queue_op(vcpu->index(), seq, [=](icicle_emulator* h) {
                            ice(icicle_map_mmio(h, address, size, read_wrapper, ptr, write_wrapper, ptr), "Failed to map MMIO");
                        });
                    }
                }
                this->kick_peers(self->index());
                return;
            }
            this->pause_peers_and([&] {
                std::lock_guard lock(this->host_view_mutex_);
                for (auto& vcpu : this->vcpus_)
                {
                    map_vm(vcpu->handle());
                }
                map_view();
            });
        }

        void map_memory(const uint64_t address, const size_t size, memory_permission permissions) override
        {
            const auto perm = static_cast<uint8_t>(permissions);
            if (this->vcpus_.size() == 1)
            {
                if (this->force_smp_memory_)
                {
                    ice(icicle_map_smp_shared_fresh(this->emu_, address, size, perm), "Failed to map SMP memory");
                }
                else
                {
                    ice(icicle_map_memory(this->emu_, address, size, perm), "Failed to map memory");
                }
                return;
            }
            auto* self = this->mutation_owner();
            const bool caller_is_vcpu = self != nullptr;
            if (!caller_is_vcpu)
            {
                // External/setup mutator: pause peers, allocate on the master, share into the rest directly.
                this->pause_peers_and([&] {
                    ice(icicle_map_smp_shared_fresh(this->emu_, address, size, perm), "Failed to map SMP memory");
                    for (size_t i = 1; i < this->vcpus_.size(); ++i)
                    {
                        ice(icicle_share_smp_pages(this->vcpus_[i]->handle(), this->emu_, address, size), "Failed to share SMP memory");
                    }
                    std::lock_guard lock(this->host_view_mutex_);
                    ice(icicle_share_smp_pages(this->host_view_, this->emu_, address, size),
                        "Failed to share SMP memory with Icicle host view");
                });
                return;
            }
            // vCPU mutator (inside its own hook, may hold the BEL): allocate on its OWN VM now, capture the
            // shared Arcs on this thread (source-safe), and queue an alias-from-capture for each peer — the
            // peer maps the captured Arcs on its own thread with NO cross-thread read of the source (6.5).
            auto* profile = this->profile_enabled_ ? &self->smp_profile_ : nullptr;
            const smp_profile_timer map_timer{profile ? &profile->map_nanos : nullptr};
            if (profile)
            {
                profile->map_calls.fetch_add(1, std::memory_order_relaxed);
            }
            ice(!t_draining_own_queue, "Cannot map SMP memory during a pending queue drain");
            this->drain_pending_unmap_prefix_before_map(*self, address, size);
            icicle_emulator* const source = self->handle();
            const fanout_guard fanout{this->ops_in_flight_};
            this->ops_in_flight_.fetch_add(1, std::memory_order_acquire);
            ice(icicle_map_smp_shared_fresh(source, address, size, perm), "Failed to map SMP memory");
            void* const raw = icicle_smp_capture(source, address, size);
            ice(raw != nullptr, "Failed to capture SMP pages");
            const std::shared_ptr<void> captured(raw, icicle_smp_release_capture);
            {
                std::lock_guard lock(this->host_view_mutex_);
                ice(icicle_smp_map_captured(this->host_view_, captured.get(), address), "Failed to map SMP pages in Icicle host view");
            }
            const uint64_t seq = this->issuer_next(self->index());
            for (auto& v : this->vcpus_)
            {
                if (v.get() != self)
                {
                    // The captured alias is applied on the peer's owning thread. Profile
                    // this cost separately from the issuer's map/capture/queue timer.
                    auto* peer_profile = this->profile_enabled_ ? &v->smp_profile_ : nullptr;
                    const auto page_count = size / 0x1000;
                    this->queue_op(v->index(), seq, [captured, address, page_count, peer_profile](icicle_emulator* h) {
                        const auto start = peer_profile ? std::chrono::steady_clock::now()
                                                        : std::chrono::steady_clock::time_point{};
                        ice(icicle_smp_map_captured(h, captured.get(), address), "Failed to map captured SMP pages");
                        if (peer_profile)
                        {
                            const auto nanos = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start).count());
                            peer_profile->peer_map_calls.fetch_add(1, std::memory_order_relaxed);
                            peer_profile->peer_map_pages.fetch_add(page_count, std::memory_order_relaxed);
                            peer_profile->peer_map_nanos.fetch_add(nanos, std::memory_order_relaxed);
                            auto maximum = peer_profile->peer_map_max_nanos.load(std::memory_order_relaxed);
                            while (maximum < nanos &&
                                   !peer_profile->peer_map_max_nanos.compare_exchange_weak(
                                       maximum, nanos, std::memory_order_relaxed))
                            {
                            }
                        }
                    });
                }
            }
            // A peer can observe pointers into this new region as soon as this syscall's guest-visible
            // effects land, so make the queued mapping apply at the peer's NEXT quantum, not after its
            // whole current one (kick_peers).
            this->kick_peers(self->index());
        }

        void map_host_memory(const uint64_t address, const size_t size, void* host_pointer, memory_permission permissions) override
        {
            const auto perm = static_cast<uint8_t>(permissions);
            const auto map_vm = [=](icicle_emulator* h) {
                ice(icicle_map_host_memory(h, address, host_pointer, size, perm), "Failed to map host memory");
            };
            if (!this->host_view_)
            {
                map_vm(this->emu_);
                return;
            }
            if (auto* self = this->mutation_owner())
            {
                {
                    std::lock_guard lock(this->host_view_mutex_);
                    map_vm(self->handle());
                    ice(icicle_map_host_memory(this->host_view_, address, host_pointer, size, perm),
                        "Failed to map caller-owned memory in Icicle host view");
                }
                const auto seq = this->issuer_next(self->index());
                for (auto& vcpu : this->vcpus_)
                {
                    if (vcpu.get() != self)
                    {
                        this->queue_op(vcpu->index(), seq, map_vm);
                    }
                }
                this->kick_peers(self->index());
                return;
            }
            this->pause_peers_and([&] {
                std::lock_guard lock(this->host_view_mutex_);
                for (auto& vcpu : this->vcpus_)
                {
                    map_vm(vcpu->handle());
                }
                ice(icicle_map_host_memory(this->host_view_, address, host_pointer, size, perm),
                    "Failed to map caller-owned memory in Icicle host view");
            });
        }

        void flush_host_memory_cache(const void* host_pointer, const size_t size) override
        {
            this->apply_to_all_vms([=](icicle_emulator* h) { icicle_flush_host_memory_cache(h, host_pointer, size); });
            if (this->host_view_)
            {
                std::lock_guard lock(this->host_view_mutex_);
                icicle_flush_host_memory_cache(this->host_view_, host_pointer, size);
            }
        }

        bool map_shared_memory(const uint64_t address, const uint64_t source, const size_t size,
                               const memory_permission permissions) override
        {
            const auto perm = static_cast<uint8_t>(permissions);
            auto* self = this->mutation_owner();
            const bool caller_is_vcpu = self != nullptr;
            if (this->vcpus_.size() == 1 || !caller_is_vcpu)
            {
                bool ok = true;
                this->pause_peers_and([&] {
                    for (auto& vcpu : this->vcpus_)
                    {
                        ok = (icicle_map_shared_memory(vcpu->handle(), address, source, size, perm) != 0) && ok;
                    }
                    if (this->host_view_)
                    {
                        std::lock_guard lock(this->host_view_mutex_);
                        ok = (icicle_map_shared_memory(this->host_view_, address, source, size, perm) != 0) && ok;
                    }
                });
                return ok;
            }
            // vCPU mutator: apply to own VM now (its result represents the coherent shared space), queue peers.
            auto* profile = this->profile_enabled_ ? &self->smp_profile_ : nullptr;
            const smp_profile_timer map_timer{profile ? &profile->map_nanos : nullptr};
            if (profile)
            {
                profile->map_calls.fetch_add(1, std::memory_order_relaxed);
            }
            const bool ok = icicle_map_shared_memory(self->handle(), address, source, size, perm) != 0;
            if (ok)
            {
                std::lock_guard lock(this->host_view_mutex_);
                ice(icicle_map_shared_memory(this->host_view_, address, source, size, perm),
                    "Failed to map shared alias in Icicle host view");
            }
            const uint64_t seq = this->issuer_next(self->index());
            for (auto& v : this->vcpus_)
            {
                if (v.get() != self)
                {
                    this->queue_op(v->index(), seq, [address, source, size, perm](icicle_emulator* h) {
                        icicle_map_shared_memory(h, address, source, size, perm);
                    });
                }
            }
            this->kick_peers(self->index());
            return ok;
        }

        void unmap_memory(const uint64_t address, const size_t size) override
        {
            if (t_running_vcpu == nullptr)
            {
                if (auto* worker = this->mutation_owner())
                {
                    // Failure-only-path proof for the former BEL/quiesce lock inversion.
                    static std::atomic<bool> reported{false};
                    if (!reported.exchange(true, std::memory_order_relaxed))
                    {
                        std::fprintf(stderr,
                                     "[SMPROUTE] scheduler_unmap vcpu=%zu address=%#llx size=%zu route=own_apply_peer_queue\n",
                                     worker->index(), static_cast<unsigned long long>(address), size);
                    }
                }
            }
            const auto unmap_vm = [=](icicle_emulator* h) {
                ice(icicle_unmap_memory(h, address, size), "Failed to unmap memory");
            };
            if (!this->host_view_)
            {
                unmap_vm(this->emu_);
                return;
            }
            const auto unmap_view = [&] {
                ice(icicle_unmap_memory(this->host_view_, address, size), "Failed to unmap Icicle host view memory");
                std::vector<std::pair<uint64_t, size_t>> remaining;
                remaining.reserve(this->mmio_ranges_.size() + 1);
                const auto unmap_end = address + size;
                for (const auto& [start, length] : this->mmio_ranges_)
                {
                    if (!ranges_overlap(address, size, start, length))
                    {
                        remaining.emplace_back(start, length);
                        continue;
                    }
                    const auto end = start + length;
                    if (start < address)
                    {
                        remaining.emplace_back(start, static_cast<size_t>(address - start));
                    }
                    if (unmap_end < end)
                    {
                        remaining.emplace_back(unmap_end, static_cast<size_t>(end - unmap_end));
                    }
                }
                this->mmio_ranges_ = std::move(remaining);
            };
            if (auto* self = this->mutation_owner())
            {
                {
                    std::lock_guard lock(this->host_view_mutex_);
                    unmap_vm(self->handle());
                    unmap_view();
                }
                const auto seq = this->issuer_next(self->index());
                for (auto& vcpu : this->vcpus_)
                {
                    if (vcpu.get() != self)
                    {
                        this->queue_op(vcpu->index(), seq, unmap_vm, std::pair{address, size});
                    }
                }
                this->kick_peers(self->index());
                return;
            }
            this->pause_peers_and([&] {
                std::lock_guard lock(this->host_view_mutex_);
                for (auto& vcpu : this->vcpus_)
                {
                    unmap_vm(vcpu->handle());
                }
                unmap_view();
            });
        }

        // 6.4 — the icicle handle to read/write guest memory through. During a syscall/hook the acting
        // vCPU (this thread's) handle is used: safe same-thread and coherent (all VMs share the smp pages),
        // instead of always the master whose VM may be executing on another thread. External threads (and
        // N=1, where the acting vCPU IS vcpus_[0]==emu_) fall back to the master.
        icicle_emulator* acting_handle() const
        {
            auto* v = this->mutation_owner();
            if (v)
            {
                return v->handle();
            }
            return this->emu_;
        }

        void report_external_mmio_refusal(const char* operation, const uint64_t address, const size_t size, const char* reason) const
        {
            static std::atomic<unsigned> emitted{0};
            if (emitted.fetch_add(1, std::memory_order_relaxed) < 8)
            {
                std::fprintf(stderr, "[ICHOSTVIEW] external-mmio-refused operation=%s address=%#llx bytes=%zu reason=%s\n", operation,
                             static_cast<unsigned long long>(address), size, reason);
            }
        }

        bool access_parked_mmio(const char* operation, const uint64_t address, const size_t size, const std::function<bool()>& access)
        {
            std::unique_lock pause(this->pause_mutex_, std::try_to_lock);
            if (!pause.owns_lock())
            {
                this->report_external_mmio_refusal(operation, address, size, "pause-in-progress");
                return false;
            }
            std::unique_lock gate(this->quiesce_mutex_, std::try_to_lock);
            if (!gate.owns_lock() || this->quiescing_ || std::any_of(this->vcpus_.begin(), this->vcpus_.end(), [](const auto& vcpu) {
                    return vcpu->run_active_.load(std::memory_order_acquire);
                }))
            {
                this->report_external_mmio_refusal(operation, address, size, "vcpu-active");
                return false;
            }
            this->quiescing_ = true;
            gate.unlock();
            const auto resume = utils::finally([this] {
                std::lock_guard lock(this->quiesce_mutex_);
                this->quiescing_ = false;
                this->quiesce_cv_.notify_all();
            });
            std::vector<std::unique_lock<std::recursive_mutex>> parked;
            parked.reserve(this->vcpus_.size());
            for (auto& vcpu : this->vcpus_)
            {
                parked.emplace_back(vcpu->parked_vm_mutex_, std::try_to_lock);
                if (!parked.back().owns_lock())
                {
                    this->report_external_mmio_refusal(operation, address, size, "worker-using-vm");
                    return false;
                }
            }
            return access();
        }

        bool overlaps_mmio(const uint64_t address, const size_t size) const
        {
            return std::any_of(this->mmio_ranges_.begin(), this->mmio_ranges_.end(),
                               [&](const auto& range) { return ranges_overlap(address, size, range.first, range.second); });
        }

        void report_host_view_hook_limit(const char* operation, const uint64_t address, const size_t size) const
        {
            static std::atomic<unsigned> emitted{0};
            if (emitted.fetch_add(1, std::memory_order_relaxed) < 8)
            {
                std::fprintf(stderr, "[ICHOSTVIEW] hook-copy-refused operation=%s address=%#llx bytes=%zu limit=%zu\n", operation,
                             static_cast<unsigned long long>(address), size, host_view_callback_limit);
            }
        }

        bool hook_callback_bytes_exceed_limit(const uint64_t address, const size_t size, const bool is_read) const
        {
            size_t budget = host_view_callback_limit;
            for (const auto& range : this->host_view_hook_ranges_)
            {
                if (range.is_read != is_read || !ranges_overlap(address, size, range.address, range.size))
                {
                    continue;
                }
                if (size > budget)
                {
                    return true;
                }
                budget -= size;
            }
            return false;
        }

        std::exception_ptr dispatch_host_view_callbacks(std::vector<std::function<void()>>& callbacks)
        {
            std::exception_ptr error;
            for (auto& callback : callbacks)
            {
                try
                {
                    callback();
                }
                catch (...)
                {
                    if (!error)
                    {
                        error = std::current_exception();
                    }
                }
            }
            return error;
        }

        bool try_read_external(const uint64_t address, void* data, const size_t size)
        {
            bool mmio = false;
            bool ok = false;
            std::vector<std::function<void()>> callbacks;
            std::exception_ptr staging_error;
            {
                std::lock_guard lock(this->host_view_mutex_);
                mmio = this->overlaps_mmio(address, size);
                if (!mmio)
                {
                    if (this->hook_callback_bytes_exceed_limit(address, size, true))
                    {
                        this->report_host_view_hook_limit("read", address, size);
                        return false;
                    }
                    ok = icicle_read_memory(this->host_view_, address, data, size) != 0;
                    callbacks.swap(this->host_view_callbacks_);
                    staging_error = std::exchange(this->host_view_callback_error_, std::exception_ptr{});
                    this->host_view_callback_bytes_ = 0;
                }
            }
            if (mmio)
            {
                return this->access_parked_mmio("read", address, size,
                                                [&] { return icicle_read_memory(this->emu_, address, data, size) != 0; });
            }
            auto callback_error = this->dispatch_host_view_callbacks(callbacks);
            if (staging_error)
            {
                std::rethrow_exception(staging_error);
            }
            if (callback_error)
            {
                std::rethrow_exception(callback_error);
            }
            return ok;
        }

        bool try_write_external(const uint64_t address, const void* data, const size_t size, bool& cached,
                                std::exception_ptr& callback_error)
        {
            bool mmio = false;
            bool ok = false;
            std::vector<std::function<void()>> callbacks;
            std::exception_ptr staging_error;
            {
                std::lock_guard lock(this->host_view_mutex_);
                mmio = this->overlaps_mmio(address, size);
                if (!mmio)
                {
                    if (this->hook_callback_bytes_exceed_limit(address, size, false))
                    {
                        this->report_host_view_hook_limit("write", address, size);
                        return false;
                    }
                    cached = icicle_host_view_code_may_be_cached(this->host_view_, address, size) != 0;
                    ok = icicle_write_memory(this->host_view_, address, data, size) != 0;
                    cached = icicle_host_view_code_may_be_cached(this->host_view_, address, size) != 0 || cached;
                    callbacks.swap(this->host_view_callbacks_);
                    staging_error = std::exchange(this->host_view_callback_error_, std::exception_ptr{});
                    this->host_view_callback_bytes_ = 0;
                }
            }
            if (mmio)
            {
                return this->access_parked_mmio("write", address, size,
                                                [&] { return icicle_write_memory(this->emu_, address, data, size) != 0; });
            }
            auto dispatch_error = this->dispatch_host_view_callbacks(callbacks);
            callback_error = staging_error ? staging_error : dispatch_error;
            return ok;
        }

        void refresh_or_protect_host_view(const uint64_t address, const size_t size, const uint8_t permissions)
        {
            size_t offset = 0;
            while (offset < size)
            {
                const auto page_address = address + offset;
                const auto length = std::min(size - offset, size_t{0x1000} - static_cast<size_t>(page_address & 0xfff));
                if (icicle_perm_epoch_of_range(this->host_view_, page_address, length) != 0)
                {
                    icicle_refresh_peer_protection(this->host_view_, page_address, length);
                }
                else
                {
                    ice(icicle_protect_memory(this->host_view_, page_address, length, permissions),
                        "Failed to protect Icicle host view memory");
                }
                offset += length;
            }
        }

        bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            if (this->host_view_ && !this->mutation_owner())
            {
                return const_cast<icicle_x86_64_emulator*>(this)->try_read_external(address, data, size);
            }
            const auto ok = icicle_read_memory(this->acting_handle(), address, data, size);
            if (ok || this->vcpus_.size() == 1)
            {
                return ok;
            }
            // 6.6: a BETWEEN-quantum worker READ races queued maps exactly like the write path
            // (the peer's loader map not yet applied on this vCPU). Drain own queue (+ bounded
            // in-flight wait) and RETRY the read. Mirrors write_memory's drain+retry closeout.
            auto* worker = t_running_vcpu == nullptr ? this->mutation_owner() : nullptr;
            if (!worker)
            {
                worker = static_cast<icicle_vcpu*>(t_worker_vcpu); // legacy direct-start fallback
            }
            if (worker && &worker->machine_ == this && t_running_vcpu == nullptr)
            {
                const_cast<icicle_x86_64_emulator*>(this)->drain_own_queue_with_inflight_wait(*worker);
                return icicle_read_memory(worker->handle(), address, data, size);
            }
            return ok;
        }

        void read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            const auto res = this->try_read_memory(address, data, size);
            ice(res, "Failed to read memory");
        }

        bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
        {
            if (this->vcpus_.size() == 1 || size == 0)
            {
                const bool ok = icicle_write_memory(this->acting_handle(), address, data, size);
                if (!ok)
                {
                    diagnose_large_write_failure(address, size);
                }
                return ok;
            }
            // NOTE: even REENTRANCY-GUARDED, a pre-write own-queue drain here SEGV'd
            // TwoVcpusExecuteConcurrentlyOverSharedCode (host AV in test body) - reverted again.
            // The guard itself stays in drain_pending_ops (hardening). The "Unmapped" between-
            // quantum write race remains open: needs op-application via the icicle C ABI that
            // never re-enters C++ write paths, or a scheduler-level drain before worker writes.

            // NOTE (5th attempt, reverted): pre-write op application destabilizes IcicleSmp
            // (6-7/8) even over per-issuer-ordered queues - the disturbance is not ordering but
            // timing/state (eager application changes when peers observe ops). The between-quantum
            // Unmapped race needs the windows_emulator to stop issuing such writes outside a
            // quantum (or provide an explicit pre-write hook) - backend-only fixes are exhausted.

            auto* self = this->mutation_owner();
            if (!self)
            {
                bool cached = false;
                std::exception_ptr callback_error;
                const bool ok = this->try_write_external(address, data, size, cached, callback_error);
                if (!ok && size == context_record_bytes && t_context_write_retry_probe &&
                    t_context_write_retry_probe->address == address)
                {
                    t_context_write_retry_probe->initial_failed = true;
                }
                if (cached)
                {
                    const uint64_t seq = this->issuer_next(this->vcpus_.size());
                    for (auto& v : this->vcpus_)
                    {
                        this->queue_invalidation(v->index(), seq, address, size);
                    }
                    this->kick_peers(std::numeric_limits<size_t>::max());
                }
                if (!ok)
                {
                    diagnose_large_write_failure(address, size);
                }
                if (callback_error)
                {
                    std::rethrow_exception(callback_error);
                }
                return ok;
            }

            const bool cached = icicle_code_range_is_cached(self->handle(), address, size);
            if (smp_trace_enabled() && cached)
            {
                std::fprintf(stderr, "[SMPTRC] write addr=%#llx size=%zu cached=1 caller=vcpu\n",
                             (unsigned long long)address, size);
            }

            const auto own = self->index();
            bool ok = icicle_write_memory(self->handle(), address, data, size);
            if (!ok)
            {
                if (size == context_record_bytes && t_context_write_retry_probe && t_context_write_retry_probe->address == address)
                {
                    t_context_write_retry_probe->initial_failed = true;
                }
                // 6.6: an IN-quantum hook write (syscall handler) races queued maps the same way
                // (the remaining untraced ...FB20 write failures land here: vCPU caller, uncached
                // range). Drain own queue (+ in-flight wait) and retry - same discipline the
                // violation wrapper already proves safe from inside a hook.
                this->drain_own_queue_with_inflight_wait(*self);
                ok = icicle_write_memory(self->handle(), address, data, size);
                if (!ok && smp_trace_enabled())
                {
                    std::fprintf(stderr, "[SMPTRC] write-RETRY-FAILED(inquantum) addr=%#llx size=%zu vcpu=%zu inflight=%llu\n",
                                 (unsigned long long)address, size, self->index(),
                                 (unsigned long long)this->ops_in_flight_.load(std::memory_order_acquire));
                }
            }
            if (cached)
            {
                const uint64_t iseq = this->issuer_next(own);
                for (auto& v : this->vcpus_)
                {
                    if (v->index() == own)
                    {
                        continue;
                    }
                    this->queue_invalidation(v->index(), iseq, address, size);
                }
                this->kick_peers(own);
            }
            if (!ok)
            {
                diagnose_large_write_failure(address, size);
            }
            return ok;
        }

        void write_memory(const uint64_t address, const void* data, const size_t size) override
        {
            context_write_retry_probe probe{address};
            const bool track_context = size == context_record_bytes && this->vcpus_.size() > 1;
            auto* const previous_probe = t_context_write_retry_probe;
            if (track_context)
            {
                t_context_write_retry_probe = &probe;
            }
            const auto restore_probe = utils::finally([&] {
                if (track_context)
                {
                    t_context_write_retry_probe = previous_probe;
                }
            });
            const auto report_retry = [&](const bool succeeded, const char* disposition, const icicle_vcpu* caller) {
                if (probe.initial_failed)
                {
                    diagnose_context_write_retry(address, caller ? static_cast<int64_t>(caller->index()) : -1, succeeded, disposition);
                }
            };
            const auto res = try_write_memory(address, data, size);
            if (res)
            {
                report_retry(true, "try-write", this->mutation_owner());
                return;
            }
            // SMP 6.6c': a host write inside a syscall hook can fail the guest-perm check when a
            // PEER's write-protect landed in the shared perm array mid-race (instant since 6.6b).
            // On real Windows this guest race takes a HANDLED access violation - dispatch it on the
            // acting vCPU instead of ice()-throwing out of the hook and killing the worker
            // ("vCPU 0 worker terminated: Failed to write memory", 0/5 probe batch). Setup-time
            // (non-vCPU) callers keep the hard throw: those failures are real host bugs.
            auto* self = static_cast<icicle_vcpu*>(t_running_vcpu);
            // NOTE: dispatching from BETWEEN-quantum writes (t_running_vcpu null) was tried and
            // SEGFAULTS - the guest dispatcher needs a real vCPU context. Those writers keep the
            // throw until the scheduler gives the backend a safe between-quantum dispatch point.
            if (self && &self->machine_ == this && this->violation_callback_)
            {
                try
                {
                    (void)this->violation_callback_(this->acting_cpu(self->index()), address, size,
                                                    memory_operation::write, memory_violation_type::protection);
                }
                catch (...)
                {
                    this->acting_sink(self->index())->defer_hook_exception(std::current_exception());
                }
                report_retry(false, "guest-fault", self);
                return;
            }
            // 6.6c'': a BETWEEN-quantum scheduler worker write (t_running_vcpu null, but
            // the explicit scheduler context still identifies its vCPU). The recurring 1232-byte CONTEXT write
            // fails Unmapped while its map is queued/in-flight; deferring only the FAULT left the
            // page zeroed (the write never retried) and ntdll later read the zero CONTEXT and
            // synthesized the terminal AV (VIENTRY=0 proved no icicle violation delivers it). So:
            // drain own queue (+ bounded in-flight wait) and RETRY THE WRITE; only if it still
            // fails, defer the fault (real guest-visible fault).
            auto* worker = t_running_vcpu == nullptr ? this->mutation_owner() : nullptr;
            if (!worker)
            {
                worker = static_cast<icicle_vcpu*>(t_worker_vcpu); // legacy direct-start fallback
            }
            if (worker && &worker->machine_ == this)
            {
                {
                    std::vector<pending_op> ops;
                    {
                        std::lock_guard<std::mutex> lock(this->pending_mutex_);
                        ops.swap(this->pending_ops_[worker->index()]);
                    }
                    if (ops.empty() && this->ops_in_flight_.load(std::memory_order_acquire) > 0)
                    {
                        for (int spin = 0; spin < 4000 && ops.empty(); ++spin)
                        {
                            std::this_thread::sleep_for(std::chrono::microseconds(50));
                            std::lock_guard<std::mutex> lock(this->pending_mutex_);
                            ops.swap(this->pending_ops_[worker->index()]);
                        }
                    }
                    for (auto& op : ops)
                    {
                        op.apply(worker->handle());
                        this->complete_op(op.ticket);
                    }
                }
                if (icicle_write_memory(worker->handle(), address, data, size))
                {
                    if (smp_trace_enabled())
                    {
                        std::fprintf(stderr, "[SMPTRC] write-RETRIED-OK addr=%#llx size=%zu vcpu=%zu\n",
                                     (unsigned long long)address, size, worker->index());
                    }
                    report_retry(true, "worker-retry", worker);
                    return;
                }
                if (this->violation_callback_)
                {
                    std::lock_guard<std::mutex> lock(this->write_faults_mutex_);
                    this->pending_write_faults_[worker->index()].emplace_back(address, size);
                    if (smp_trace_enabled())
                    {
                        std::fprintf(stderr, "[SMPTRC] write-DEFERRED addr=%#llx size=%zu vcpu=%zu tid=%u\n",
                                     (unsigned long long)address, size, worker->index(),
                                     (unsigned)(GetCurrentThreadId()));
                    }
                    report_retry(false, "deferred-fault", worker);
                    return;
                }
            }
            if (smp_trace_enabled())
            {
                std::fprintf(stderr, "[SMPTRC] write-FAIL ice-throw addr=%#llx size=%zu tid=%u\n",
                             (unsigned long long)address, size, (unsigned)(GetCurrentThreadId()));
            }
            report_retry(false, "throw", worker);
            ice(false, "Failed to write memory");
        }

        void apply_memory_protection(const uint64_t address, const size_t size, memory_permission permissions) override
        {
            const auto perm = static_cast<uint8_t>(permissions);
            auto* self = this->mutation_owner();
            const bool caller_is_vcpu = self != nullptr;
            if (this->vcpus_.size() == 1)
            {
                ice(icicle_protect_memory(this->emu_, address, size, perm), "Failed to apply permissions");
                return;
            }
            if (!caller_is_vcpu)
            {
                this->pause_peers_and([&] {
                    std::lock_guard lock(this->host_view_mutex_);
                    for (auto& vcpu : this->vcpus_)
                    {
                        ice(icicle_protect_memory(vcpu->handle(), address, size, perm), "Failed to apply permissions");
                    }
                    this->refresh_or_protect_host_view(address, size, perm);
                });
                return;
            }
            // Protecting an SMP-shared page changes the one shared PageData immediately.
            // Peers must drop their own TLB/JIT translations, but must not replay the old
            // permission write: that can overwrite a newer protection or a remapped page.
            const auto own = self->index();
            auto* profile = this->profile_enabled_ ? &self->smp_profile_ : nullptr;
            const smp_profile_timer protect_timer{profile ? &profile->protect_nanos : nullptr};
            if (profile)
            {
                profile->protect_calls.fetch_add(1, std::memory_order_relaxed);
            }
            bool all_smp_shared = false;
            {
                std::lock_guard lock(this->host_view_mutex_);
                all_smp_shared = icicle_perm_epoch_of_range(self->handle(), address, size) != 0;
                ice(icicle_protect_memory(self->handle(), address, size, perm), "Failed to apply permissions");
                this->refresh_or_protect_host_view(address, size, perm);
            }
            const uint64_t pseq = this->issuer_next(own);
            for (auto& v : this->vcpus_)
            {
                if (v->index() != own)
                {
                    this->queue_op(v->index(), pseq, [address, size, perm, all_smp_shared](icicle_emulator* h) {
                        if (all_smp_shared)
                        {
                            icicle_refresh_peer_protection(h, address, size);
                        }
                        else
                        {
                            // Mixed ranges need per-page handling. Never replay an old permission
                            // write over a shared page, even if another page in the range is local.
                            // Local mappings still require their own permission update.
                            size_t offset = 0;
                            while (offset < size)
                            {
                                const auto page_address = address + offset;
                                const auto length = std::min(size - offset,
                                                             size_t{0x1000} - static_cast<size_t>(page_address & 0xfff));
                                if (icicle_perm_epoch_of_range(h, page_address, length) != 0)
                                {
                                    icicle_refresh_peer_protection(h, page_address, length);
                                }
                                else
                                {
                                    ice(icicle_protect_memory(h, page_address, length, perm), "Failed to apply permissions");
                                }
                                offset += length;
                            }
                        }
                    });
                }
            }
            this->kick_peers(own);
        }

        // The cpu_interface a hook on VM `index` reports to its callback. VM 0 is the machine itself
        // (emu.get()) -- the historical cpu-0 identity that hooks are expected to carry (mirrors the
        // pre-SMP behavior and matches icicle_execution_hook_test); extra VMs report their own vCPU so a
        // callback sees the acting vCPU under SMP.
        cpu_interface& acting_cpu(const size_t index)
        {
            return index == 0 ? static_cast<cpu_interface&>(*this) : static_cast<cpu_interface&>(*this->vcpus_[index]);
        }

        // The sink a hook on VM `index` defers exceptions to. VM 0 uses the machine (which forwards to
        // vcpus_[0]); extra VMs use their own vCPU. Either way the exception surfaces from that VM's start().
        detail::hook_exception_sink* acting_sink(const size_t index)
        {
            return index == 0 ? static_cast<detail::hook_exception_sink*>(this) : this->vcpus_[index].get();
        }

        // The raw icicle hook wrappers are captureless function pointers, so the triggering vCPU is bound
        // into the stored function up front. With N VMs the same hook is registered on each and bound to
        // that VM's acting cpu (see acting_cpu), so callbacks see the acting vCPU and defer exceptions to it.
        template <typename Ret, typename... Args>
        std::function<Ret(Args...)> bind_cpu(const size_t vcpu_index, std::function<Ret(cpu_interface&, Args...)> callback)
        {
            return [this, vcpu_index, c = std::move(callback)](Args... args) -> Ret {
                try
                {
                    if constexpr (std::is_void_v<Ret>)
                    {
                        c(this->acting_cpu(vcpu_index), std::forward<Args>(args)...);
                    }
                    else
                    {
                        return c(this->acting_cpu(vcpu_index), std::forward<Args>(args)...);
                    }
                }
                catch (...)
                {
                    // Never let a host exception reach icicle's Rust frames (hook_exception_sink).
                    // The default result (stop / run_instruction / 0) is harmless: icicle stops now.
                    this->acting_sink(vcpu_index)->defer_hook_exception(std::current_exception());
                    if constexpr (!std::is_void_v<Ret>)
                    {
                        return Ret{};
                    }
                }
            };
        }

        emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override
        {
            const auto kind = static_cast<x86_hookable_instructions>(instruction_type);
            if (kind != x86_hookable_instructions::syscall && kind != x86_hookable_instructions::rdtsc &&
                kind != x86_hookable_instructions::rdtscp)
            {
                return nullptr;
            }

            auto* handle = this->fresh_hook_handle();
            auto reg = std::make_shared<hook_registration>();
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                // 6.6c NOTE: a syscall-exit drain of this vCPU's pending queue was tried and
                // REVERTED (probe 0/8): applying a peer's queued UNMAP mid-hook breaks the running
                // syscall's own subsequent guest writes ("Failed to write memory"). Closing the
                // deferred-protect DEP window needs an op-type-filtered drain (perm-only) at a
                // post-block safe point - see SMP-PLAN 6.6c.
                auto obj = make_function_object(this->bind_cpu(i, callback));
                auto* ptr = obj.get();

                const auto invoker = +[](void* cb) {
                    const auto& func = *static_cast<decltype(ptr)>(cb);
                    (void)func(0); //
                };

                const auto timestamp_invoker = +[](void* cb) -> uint32_t {
                    const auto& func = *static_cast<decltype(ptr)>(cb);
                    return static_cast<uint32_t>(func(0));
                };
                auto* const vm = this->vcpus_[i]->handle();
                const auto id = kind == x86_hookable_instructions::syscall
                                    ? icicle_add_syscall_hook(vm, invoker, ptr)
                                    : icicle_add_timestamp_hook(vm, kind == x86_hookable_instructions::rdtscp, timestamp_invoker, ptr);
                reg->entries.emplace_back(i, id, std::move(obj));
            }

            std::unique_lock lock(this->partition_mutex_);
            this->registrations_[handle] = std::move(reg);
            return handle;
        }

        emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
        {
            auto* handle = this->fresh_hook_handle();
            auto reg = std::make_shared<hook_registration>();
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto object = make_function_object(this->bind_cpu(i, callback));
                auto* ptr = object.get();
                auto* wrapper = +[](void* user, const uint64_t addr, const uint64_t instructions) {
                    const hook_scope exec_scope(&detail::in_execution_hook_flag()); // fires under execution_hooks
                    basic_block block{};
                    block.address = addr;
                    block.instruction_count = static_cast<size_t>(instructions);

                    const auto& func = *static_cast<decltype(ptr)>(user);
                    (func)(block);
                };

                const auto id = icicle_add_block_hook(this->vcpus_[i]->handle(), wrapper, ptr);
                reg->entries.emplace_back(i, id, std::move(object));
            }

            std::unique_lock lock(this->partition_mutex_);
            this->registrations_[handle] = std::move(reg);
            return handle;
        }

        emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
        {
            auto* handle = this->fresh_hook_handle();
            auto reg = std::make_shared<hook_registration>();
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto obj = make_function_object(this->bind_cpu(i, callback));
                auto* ptr = obj.get();
                auto* wrapper = +[](void* user, const int32_t code) {
                    const auto& func = *static_cast<decltype(ptr)>(user);
                    func(code);
                };

                const auto id = icicle_add_interrupt_hook(this->vcpus_[i]->handle(), wrapper, ptr);
                reg->entries.emplace_back(i, id, std::move(obj));
            }

            std::unique_lock lock(this->partition_mutex_);
            this->registrations_[handle] = std::move(reg);
            return handle;
        }

        emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
        {
            // SMP 6.6c': kept so a racing HOST write that fails the guest-perm check can dispatch
            // the guest AV path on the acting vCPU instead of hard-throwing out of the syscall hook.
            this->violation_callback_ = callback;
            // SMP 6.6 FINAL ITEM - fault-driven lazy application: a GUEST access that faults
            // UNMAPPED while this vCPU still has queued cross-VM ops (the loader's async maps -
            // the probe's terminal ntdll read-AV at ...FB20 is exactly this: the CONTEXT page read
            // before the peer's map lands) drains its OWN queue and RESTARTS the instruction. The
            // restarted access sees the landed map. No eager drains -> no timing perturbation;
            // drains happen only when a real fault meets pending ops.
            memory_violation_hook_callback wrapped = [this, cb = callback](cpu_interface& cpu, const uint64_t address, const size_t size,
                                                            const memory_operation operation,
                                                            const memory_violation_type type) -> memory_violation_continuation {
                // Both fault classes race queued ops: UNMAPPED (map not yet applied) and
                // PROTECTION (protect not yet applied - page mapped with restrictive perms).
                if (type == memory_violation_type::unmapped || type == memory_violation_type::protection)
                {
                    auto* running = static_cast<icicle_vcpu*>(t_running_vcpu);
                    auto* worker = static_cast<icicle_vcpu*>(t_worker_vcpu);
                    icicle_vcpu* v = (running && &running->machine_ == this) ? running : worker;
                    if (v && &v->machine_ == this)
                    {
                        std::vector<pending_op> ops;
                        {
                            std::lock_guard<std::mutex> lock(this->pending_mutex_);
                            ops.swap(this->pending_ops_[v->index()]);
                        }
                        if (ops.empty() && this->ops_in_flight_.load(std::memory_order_acquire) > 0)
                        {
                            // A peer is mid-fanout (own-VM applied, peer queue not yet filled) -
                            // exactly the terminal-fault window (VIORESTART=0 + SMPDIAG=1 runs).
                            // Bounded wait for the in-flight op to land, then restart.
                            for (int spin = 0; spin < 4000 && ops.empty(); ++spin)
                            {
                                std::this_thread::sleep_for(std::chrono::microseconds(50));
                                std::lock_guard<std::mutex> lock(this->pending_mutex_);
                                ops.swap(this->pending_ops_[v->index()]);
                            }
                        }
                        if (!ops.empty())
                        {
                            std::fprintf(stderr, "[VIORESTART] drained %zu ops on vcpu %zu for addr=%#llx type=%d, restarting instruction\n",
                                         ops.size(), v->index(), (unsigned long long)address, (int)type);
                            for (auto& op : ops)
                            {
                                op.apply(v->handle());
                                this->complete_op(op.ticket);
                            }
                            return memory_violation_continuation::restart;
                        }
                        if (smp_trace_enabled())
                        {
                            std::fprintf(stderr, "[VIORESTART-declined] type=%d addr=%#llx q-empty%s\n", (int)type,
                                         (unsigned long long)address,
                                         this->ops_in_flight_.load(std::memory_order_acquire) > 0 ? "+inflight" : "");
                        }
                    }
                }
                return cb(cpu, address, size, operation, type);
            };
            auto* handle = this->fresh_hook_handle();
            auto reg = std::make_shared<hook_registration>();
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto obj = make_function_object(this->bind_cpu(i, wrapped));
                auto* ptr = obj.get();
                auto* wrapper = +[](void* user, const uint64_t address, const uint8_t operation, const int32_t unmapped) -> int32_t {
                    const auto violation_type = unmapped //
                                                    ? memory_violation_type::unmapped
                                                    : memory_violation_type::protection;

                    const auto& func = *static_cast<decltype(ptr)>(user);
                    const auto res = func(address, 1, static_cast<memory_operation>(operation), violation_type);
                    const auto restart = res == memory_violation_continuation::restart;
                    const auto resume = res == memory_violation_continuation::resume || restart;
                    return resume ? 1 : 0;
                };

                const auto id = icicle_add_violation_hook(this->vcpus_[i]->handle(), wrapper, ptr);
                reg->entries.emplace_back(i, id, std::move(obj));
            }

            std::unique_lock lock(this->partition_mutex_);
            this->registrations_[handle] = std::move(reg);
            return handle;
        }

        emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
        {
            if (detail::in_hook_flag() && detail::in_execution_hook_flag())
            {
                // 6.5: requested from INSIDE an execution-family hook — installing one right there panics
                // icicle's execution_hooks RefCell, and windows_emulator may hold the BEL here. Defer to
                // this vCPU's next quantum boundary (kick_self), draining outside run().
                return this->defer_exec_hook_install(address, 1, std::move(callback));
            }
            // From a syscall/read/write hook or external context: execution_hooks is free — install now
            // via the routing (a vCPU caller registers its own VM immediately; no peer pause).
            auto* handle = this->fresh_hook_handle();
            this->install_exec_hook(address, 1, callback, handle);
            return handle;
        }

        emulator_hook* hook_memory_range_execution(const uint64_t address, const uint64_t size,
                                                   memory_execution_hook_callback callback) override
        {
            if (size == 1)
            {
                // Delegates to the exact-address hook; do that BEFORE any locking here.
                return this->hook_memory_execution(address, std::move(callback));
            }

            if (detail::in_hook_flag() && detail::in_execution_hook_flag())
            {
                return this->defer_exec_hook_install(address, size, std::move(callback));
            }
            auto* handle = this->fresh_hook_handle();
            this->install_exec_hook(address, size, callback, handle);
            return handle;
        }

        // Installs an exact/ranged execution hook on every VM via the 6.5 routing rule: a vCPU caller
        // registers on its own VM now and queues peers; an external caller pauses peers. Either way each
        // VM is only ever touched from a context where that is safe.
        void install_exec_hook(const uint64_t address, const uint64_t size, const memory_execution_hook_callback& callback,
                               emulator_hook* handle)
        {
            auto reg = std::make_shared<hook_registration>();
            {
                std::unique_lock lock(this->partition_mutex_);
                this->registrations_[handle] = reg;
            }

            this->route_to_all_vms([this, address, size, callback, reg](const size_t i) {
                if (reg->deleted)
                {
                    return; // deleted before this VM's install drained
                }
                auto object = std::make_unique<detail::execution_hook>(this->acting_cpu(i), callback, this->acting_sink(i));
                auto* ptr = object.get();
                auto* wrapper = +[](void* user, const uint64_t addr) {
                    const auto& func = *static_cast<decltype(ptr)>(user);
                    (func)(addr);
                };

                const auto id = size == 1
                                    ? icicle_add_execution_hook(this->vcpus_[i]->handle(), address, wrapper, ptr)
                                    : icicle_add_ranged_execution_hook(this->vcpus_[i]->handle(), address, size, wrapper, ptr);
                std::unique_lock lock(this->partition_mutex_);
                reg->entries.emplace_back(i, id, std::move(object));
            });
        }

        // 6.5: reserve the handle and install the execution hook when this vCPU's CURRENT QUANTUM ends
        // (self-kick: icicle exits at the next block boundary), draining in begin_run_quantum — OUTSIDE
        // icicle run(), where execution_hooks is not borrowed. (The run_on_next_instruction one-shot
        // itself lives in execution_hooks, so installing an execution hook from inside it panics with
        // "RefCell already borrowed" — caught by RangedExecHookDeferredFromInHookContextWithPeerParkedInHook.)
        // Read/write hooks are separate RefCells and keep using the one-shot (try_install_memory_access_hook).
        emulator_hook* defer_exec_hook_install(const uint64_t address, const uint64_t size,
                                               memory_execution_hook_callback callback)
        {
            auto* self = static_cast<icicle_vcpu*>(t_running_vcpu);
            auto* hook_id = this->fresh_hook_handle();
            {
                std::unique_lock lock(this->partition_mutex_);
                auto reg = std::make_shared<hook_registration>();
                reg->pending = true;
                this->registrations_[hook_id] = std::move(reg);
                this->exec_hooks_to_install_[hook_id] = {.address = address, .size = size, .callback = std::move(callback)};
            }

            if (self && &self->machine_ == this)
            {
                this->kick_self(self->index_);
            }
            return hook_id;
        }

        emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
        {
            // Generic (every-instruction) execution hook: only installed at setup (instruction precision /
            // preemption), where all VMs are idle — direct registration on every handle is safe.
            auto* handle = this->fresh_hook_handle();
            auto reg = std::make_shared<hook_registration>();
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto object = std::make_unique<detail::execution_hook>(this->acting_cpu(i), callback, this->acting_sink(i));
                auto* ptr = object.get();
                auto* wrapper = +[](void* user, const uint64_t addr) {
                    const auto& func = *static_cast<decltype(ptr)>(user);
                    (func)(addr);
                };

                const auto id = icicle_add_generic_execution_hook(this->vcpus_[i]->handle(), wrapper, ptr);
                reg->entries.emplace_back(i, id, std::move(object));
            }
            std::unique_lock lock(this->partition_mutex_);
            this->registrations_[handle] = std::move(reg);

            return handle;
        }

        emulator_hook* hook_memory_read(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
        {
            return this->try_install_memory_access_hook(memory_access_hook{
                .address = address,
                .size = size,
                .callback = std::move(callback),
                .is_read = true,
            });
        }

        emulator_hook* hook_memory_write(const uint64_t address, const uint64_t size, memory_access_hook_callback callback) override
        {
            return this->try_install_memory_access_hook(memory_access_hook{
                .address = address,
                .size = size,
                .callback = std::move(callback),
                .is_read = false,
            });
        }

        emulator_hook* hook_memory_write_observed(const uint64_t address, const uint64_t size,
                                                  memory_write_observation_callback callback) override
        {
            return this->try_install_memory_access_hook(memory_access_hook{
                .address = address,
                .size = size,
                .observation = std::move(callback),
            });
        }

        void delete_hook(emulator_hook* hook) override
        {
            if (detail::in_hook_flag())
            {
                // Inside a running hook: icicle is iterating its hook tables on this thread, and the
                // windows_emulator may hold the BEL — defer to quantum end, where the 6.5 routing applies
                // the removals without pausing anyone.
                std::shared_ptr<hook_registration> reg;
                {
                    std::unique_lock lock(this->partition_mutex_);
                    this->hooks_to_delete_.insert(hook);
                    if (const auto it = this->registrations_.find(hook); it != this->registrations_.end())
                    {
                        reg = it->second;
                    }
                }
                if (reg)
                {
                    this->cancel_host_view_callback(reg, false);
                }
                return;
            }
            this->delete_hook_internal(hook);
        }

        void serialize_state(utils::buffer_serializer& buffer, const bool is_snapshot) const override
        {
            if (icicle_has_host_mappings(this->emu_))
            {
                throw std::runtime_error("Cannot save or restore Icicle state while caller-owned host memory is mapped");
            }
            if (is_snapshot && this->vcpus_.size() > 1)
            {
                throw std::runtime_error("Icicle snapshots are not supported with multiple vCPUs");
            }
            if (is_snapshot)
            {
                const auto snapshot = icicle_create_snapshot(this->emu_);
                buffer.write<uint32_t>(snapshot);
            }
            else
            {
                buffer.write_vector(this->save_registers());
            }
        }

        void deserialize_state(utils::buffer_deserializer& buffer, const bool is_snapshot) override
        {
            if (icicle_has_host_mappings(this->emu_))
            {
                throw std::runtime_error("Cannot save or restore Icicle state while caller-owned host memory is mapped");
            }
            if (is_snapshot && this->vcpus_.size() > 1)
            {
                throw std::runtime_error("Icicle snapshots are not supported with multiple vCPUs");
            }
            if (is_snapshot)
            {
                const auto snapshot = buffer.read<uint32_t>();
                icicle_restore_snapshot(this->emu_, snapshot);
            }
            else
            {
                icicle_reset_volatile_state(this->emu_);
                const auto data = buffer.read_vector<std::byte>();
                this->restore_registers(data);
            }
        }

        std::vector<std::byte> save_registers() const override
        {
            return this->vcpus_[0]->save_registers();
        }

        void restore_registers(const std::vector<std::byte>& register_data) override
        {
            this->vcpus_[0]->restore_registers(register_data);
        }

        bool has_violation() const override
        {
            return this->vcpus_[0]->has_violation();
        }

        bool supports_instruction_counting() const override
        {
            return true;
        }

        bool is_stop_thread_safe() const override
        {
            return true;
        }

        bool supports_multiple_vcpus() const override
        {
            return this->vcpus_.size() > 1;
        }

        std::string get_name() const override
        {
            return "icicle-emu";
        }

        // SMP scheduler integration: apply everything queued for the vCPU this OS thread owns.
        // Called from the windows_emulator worker between quanta (kernel lock held, outside any
        // icicle run and outside any write path) - the safe point five write-path attempts could
        // not synthesize. Fixes the between-quantum 'Unmapped' race (the recurring 1232-byte
        // CONTEXT write during thread switches hitting a map queued but not yet applied).
        uint64_t smp_op_watermark() const override
        {
            return this->ops_issued_watermark_.load(std::memory_order_acquire);
        }

        bool smp_op_applied(const uint64_t mark) const override
        {
            if (this->vcpus_.size() == 1)
            {
                return true;
            }
            std::lock_guard<std::mutex> lock(this->pending_mutex_);
            return this->ops_completed_watermark_ >= mark;
        }

        void sync_worker_context(const size_t vcpu_index) override
        {
            // The scheduler passes the authoritative vCPU index: a worker that has not run its
            // first quantum yet has no thread-local registration, and its idle loop must still
            // drain its own queue (GATEDIAG livelock at N>=4: q_i stuck forever, all parked).
            if (vcpu_index < this->vcpus_.size())
            {
                auto& worker = *this->vcpus_[vcpu_index];
                std::lock_guard<std::recursive_mutex> parked_owner(worker.parked_vm_mutex_);
                this->drain_pending_ops(worker);
                // Progress-meter safe point: the caller owns this vCPU and its VM is parked
                // here, so both reads are race-free.
                worker.published_instructions_.store(icicle_get_icount(worker.emu_), std::memory_order_relaxed);
                worker.published_rip_.store(worker.read_instruction_pointer(), std::memory_order_relaxed);
                if (this->jit_profile_enabled_)
                {
                    icicle_jit_profile p{};
                    if (icicle_get_jit_profile(worker.emu_, &p))
                    {
                        worker.jit_compile_calls_.store(p.compile_calls, std::memory_order_relaxed);
                        worker.jit_compile_nanos_.store(p.compile_nanos, std::memory_order_relaxed);
                        worker.jit_reset_calls_.store(p.reset_calls, std::memory_order_relaxed);
                        worker.jit_recompile_calls_.store(p.recompile_calls, std::memory_order_relaxed);
                        worker.jit_recompile_nanos_.store(p.recompile_nanos, std::memory_order_relaxed);
                        worker.jit_recompile_compile_calls_.store(p.recompile_compile_calls, std::memory_order_relaxed);
                        worker.jit_recompile_compile_nanos_.store(p.recompile_compile_nanos, std::memory_order_relaxed);
                        worker.jit_reset_generation_.store(p.reset_generation, std::memory_order_relaxed);
                        worker.jit_reset_cause_flags_.store(p.reset_cause_flags, std::memory_order_relaxed);
                        worker.jit_reset_manual_origin_flags_.store(p.reset_manual_origin_flags, std::memory_order_relaxed);
                        worker.jit_flush_code_nanos_.store(p.flush_code_nanos, std::memory_order_relaxed);
                        worker.jit_reset_nanos_.store(p.jit_reset_nanos, std::memory_order_relaxed);
                        worker.jit_generation_compile_calls_.store(p.generation_compile_calls, std::memory_order_relaxed);
                        worker.jit_generation_compile_nanos_.store(p.generation_compile_nanos, std::memory_order_relaxed);
                        worker.jit_origin_first_address_compiles_.store(p.origin_first_address_compiles, std::memory_order_relaxed);
                        worker.jit_origin_repeat_after_reset_compiles_.store(p.origin_repeat_after_reset_compiles, std::memory_order_relaxed);
                        worker.jit_origin_repeat_in_generation_compiles_.store(p.origin_repeat_in_generation_compiles, std::memory_order_relaxed);
                        worker.jit_origin_periodic_recompile_compiles_.store(p.origin_periodic_recompile_compiles, std::memory_order_relaxed);
                        worker.jit_origin_unclassified_compiles_.store(p.origin_unclassified_compiles, std::memory_order_relaxed);
                        worker.jit_origin_generation_number_.store(p.origin_generation_number, std::memory_order_relaxed);
                    }
                }
            }
        }

        std::vector<vcpu_activity_snapshot> vcpu_activity() const override
        {
            std::vector<vcpu_activity_snapshot> out{};
            out.reserve(this->vcpus_.size());
            for (const auto& vcpu : this->vcpus_)
            {
                out.push_back(vcpu_activity_snapshot{
                    .instructions = vcpu->published_instructions_.load(std::memory_order_relaxed),
                    .rip = vcpu->published_rip_.load(std::memory_order_relaxed),
                });
            }
            return out;
        }

        std::vector<smp_profile_snapshot> smp_profile() const override
        {
            if (!this->profile_enabled_ && !this->exec_write_wake_enabled_)
            {
                return {};
            }
            std::vector<smp_profile_snapshot> out;
            out.reserve(this->vcpus_.size());
            for (const auto& vcpu : this->vcpus_)
            {
                const auto& p = vcpu->smp_profile_;
                icicle_exec_write_wake_profile wake{};
                icicle_exec_write_filter_profile filter{};
                icicle_invalidation_profile invalidation{};
                icicle_manual_invalidation_profile manual{};
                if (this->profile_enabled_)
                {
                    ice(icicle_get_invalidation_profile(vcpu->handle(), &invalidation) != 0,
                        "Failed to read Icicle invalidation profile");
                    ice(icicle_get_manual_invalidation_profile(vcpu->handle(), &manual) != 0,
                        "Failed to read Icicle manual invalidation profile");
                }
                if (this->exec_write_wake_enabled_)
                {
                    ice(icicle_get_exec_write_wake_profile(vcpu->handle(), &wake) != 0,
                        "Failed to read Icicle executable-write wake counters");
                    ice(icicle_get_exec_write_filter_profile(vcpu->handle(), &filter) != 0,
                        "Failed to read Icicle executable-write filter counters");
                }
                out.push_back(smp_profile_snapshot{
                    .map_calls = p.map_calls.load(std::memory_order_relaxed),
                    .map_nanos = p.map_nanos.load(std::memory_order_relaxed),
                    .peer_map_calls = p.peer_map_calls.load(std::memory_order_relaxed),
                    .peer_map_pages = p.peer_map_pages.load(std::memory_order_relaxed),
                    .peer_map_nanos = p.peer_map_nanos.load(std::memory_order_relaxed),
                    .peer_map_max_nanos = p.peer_map_max_nanos.load(std::memory_order_relaxed),
                    .protect_calls = p.protect_calls.load(std::memory_order_relaxed),
                    .protect_nanos = p.protect_nanos.load(std::memory_order_relaxed),
                    .queue_ops = p.queue_ops.load(std::memory_order_relaxed),
                    .queue_nanos = p.queue_nanos.load(std::memory_order_relaxed),
                    .invalidate_queued = p.invalidate_queued.load(std::memory_order_relaxed),
                    .invalidate_queue_nanos = p.invalidate_queue_nanos.load(std::memory_order_relaxed),
                    .invalidate_applied = p.invalidate_applied.load(std::memory_order_relaxed),
                    .invalidate_no_change = p.invalidate_no_change.load(std::memory_order_relaxed),
                    .invalidate_apply_nanos = p.invalidate_apply_nanos.load(std::memory_order_relaxed),
                    .invalidate_adjacent_same_pages = p.invalidate_adjacent_same_pages.load(std::memory_order_relaxed),
                    .kick_calls = p.kick_calls.load(std::memory_order_relaxed),
                    .kick_targets = p.kick_targets.load(std::memory_order_relaxed),
                    .kick_nanos = p.kick_nanos.load(std::memory_order_relaxed),
                    .exec_write_wake_enabled = this->exec_write_wake_enabled_,
                    .exec_write_wake_events_total = wake.wake_events,
                    .exec_write_guest_jit_total = wake.guest_jit_writes,
                    .exec_write_guest_mmu_total = wake.guest_mmu_writes,
                    .exec_write_host_total = wake.host_writes,
                    .exec_write_owner_flushes_total = wake.owner_flushes,
                    .exec_write_filtered_noncode_total = filter.filtered_noncode,
                    .exec_write_notified_overlap_total = filter.notified_overlap,
                    .exec_write_notified_concurrent_total = filter.notified_concurrent,
                    .invalidation_profile_enabled = this->profile_enabled_,
                    .epoch_mismatches_total = invalidation.epoch_mismatches,
                    .jit_resets_total = invalidation.jit_resets,
                    .jit_reset_epoch_total = invalidation.epoch_resets,
                    .jit_reset_wake_total = invalidation.wake_resets,
                    .jit_reset_manual_total = invalidation.manual_resets,
                    .jit_reset_mixed_total = invalidation.mixed_resets,
                    .jit_reset_unknown_total = invalidation.unknown_resets,
                    .manual_origin_resets_total = manual.total,
                    .manual_peer_protection_total = manual.peer_protection,
                    .manual_public_invalidate_total = manual.public_invalidate,
                    .manual_self_modifying_total = manual.self_modifying,
                    .manual_host_cache_total = manual.host_cache,
                    .manual_unmap_total = manual.unmap,
                    .manual_protect_total = manual.protect,
                    .manual_host_write_total = manual.host_write,
                    .manual_multiple_origins_total = manual.multiple_origins,
                    .manual_unknown_origin_total = manual.unknown_origin,
                });
            }
            return out;
        }

        std::vector<jit_profile_snapshot> jit_profile() const override
        {
            if (!this->jit_profile_enabled_)
            {
                return {};
            }
            std::vector<jit_profile_snapshot> out;
            out.reserve(this->vcpus_.size());
            for (const auto& vcpu : this->vcpus_)
            {
                out.push_back(jit_profile_snapshot{
                    .compile_calls = vcpu->jit_compile_calls_.load(std::memory_order_relaxed),
                    .compile_nanos = vcpu->jit_compile_nanos_.load(std::memory_order_relaxed),
                    .reset_calls = vcpu->jit_reset_calls_.load(std::memory_order_relaxed),
                    .recompile_calls = vcpu->jit_recompile_calls_.load(std::memory_order_relaxed),
                    .recompile_nanos = vcpu->jit_recompile_nanos_.load(std::memory_order_relaxed),
                    .recompile_compile_calls = vcpu->jit_recompile_compile_calls_.load(std::memory_order_relaxed),
                    .recompile_compile_nanos = vcpu->jit_recompile_compile_nanos_.load(std::memory_order_relaxed),
                    .reset_generation = vcpu->jit_reset_generation_.load(std::memory_order_relaxed),
                    .reset_cause_flags = vcpu->jit_reset_cause_flags_.load(std::memory_order_relaxed),
                    .reset_manual_origin_flags = vcpu->jit_reset_manual_origin_flags_.load(std::memory_order_relaxed),
                    .flush_code_nanos = vcpu->jit_flush_code_nanos_.load(std::memory_order_relaxed),
                    .jit_reset_nanos = vcpu->jit_reset_nanos_.load(std::memory_order_relaxed),
                    .generation_compile_calls = vcpu->jit_generation_compile_calls_.load(std::memory_order_relaxed),
                    .generation_compile_nanos = vcpu->jit_generation_compile_nanos_.load(std::memory_order_relaxed),
                    .origin_first_address_compiles = vcpu->jit_origin_first_address_compiles_.load(std::memory_order_relaxed),
                    .origin_repeat_after_reset_compiles = vcpu->jit_origin_repeat_after_reset_compiles_.load(std::memory_order_relaxed),
                    .origin_repeat_in_generation_compiles = vcpu->jit_origin_repeat_in_generation_compiles_.load(std::memory_order_relaxed),
                    .origin_periodic_recompile_compiles = vcpu->jit_origin_periodic_recompile_compiles_.load(std::memory_order_relaxed),
                    .origin_unclassified_compiles = vcpu->jit_origin_unclassified_compiles_.load(std::memory_order_relaxed),
                    .origin_generation_number = vcpu->jit_origin_generation_number_.load(std::memory_order_relaxed),
                });
            }
            return out;
        }

        bool has_deterministic_instruction_count() const override
        {
            return true; // icicle maintains cpu.icount via fuel accounting even in lean JIT mode
        }

        uint64_t executed_instructions_total() const override
        {
            uint64_t total = 0;
            for (const auto& vcpu : this->vcpus_)
            {
                // Peers may be executing: u64 reads are advisory-stale, which is fine - the
                // sum only ever grows, so callers get a monotonic time source either way.
                total += icicle_get_icount(vcpu->emu_);
            }
            return total;
        }

        std::string smp_gate_debug() const override
        {
            std::string out{};
            char buf[48];
            {
                std::lock_guard<std::mutex> lock(this->pending_mutex_);
                for (size_t i = 0; i < this->pending_ops_.size(); ++i)
                {
                    std::snprintf(buf, sizeof(buf), "q%zu=%zu ", i, this->pending_ops_[i].size());
                    out += buf;
                }
            }
            const auto issued = this->ops_issued_watermark_.load(std::memory_order_acquire);
            std::snprintf(buf, sizeof(buf), "issued=%llu ", static_cast<unsigned long long>(issued));
            out += buf;
            for (const auto& vcpu : this->vcpus_)
            {
                std::snprintf(buf, sizeof(buf), "v%u=%s ", vcpu->index_, vcpu->run_active_.load() ? "RUN" : "park");
                out += buf;
            }
            {
                std::lock_guard<std::mutex> lock(this->quiesce_mutex_);
                out += this->quiescing_ ? "quiescing=1" : "quiescing=0";
            }
            return out;
        }

      private:
        icicle_vcpu* mutation_owner() const
        {
            auto* running = static_cast<icicle_vcpu*>(t_running_vcpu);
            if (running && &running->machine_ == this)
            {
                return running;
            }
            auto* worker = static_cast<icicle_vcpu*>(t_scheduler_worker_vcpu);
            return worker && &worker->machine_ == this ? worker : nullptr;
        }

        std::list<std::unique_ptr<utils::object>> storage_{};

        // One hook API handle maps to one icicle registration per vCPU VM: (vm index, icicle hook id, and
        // the object kept alive for that VM's captureless wrapper). delete_hook removes every VM's
        // registration; at N=1 this is exactly the old single (id, object) pair.
        // Shared ownership (6.5): peer-queued async ops hold the registration while it drains, so a map
        // erase can't dangle it; `deleted` makes a late-draining install a no-op instead of resurrecting a
        // hook the API user already deleted. entries is mutated from several vCPU threads → always under
        // partition_mutex_.
        struct hook_registration
        {
            std::vector<std::tuple<size_t, uint32_t, std::unique_ptr<utils::object>>> entries{};
            bool pending{false}; // reserved handle for a hook queued from inside a running hook
            bool deleted{false}; // delete_hook seen; queued installs must not apply
            std::mutex callback_mutex{};
            std::condition_variable callback_finished{};
            bool callback_cancelled{false};
            size_t active_callbacks{};
        };

        inline static thread_local const hook_registration* active_host_view_callback_ = nullptr;

        template <typename Fn>
        void invoke_host_view_callback(const std::shared_ptr<hook_registration>& reg, Fn&& callback)
        {
            {
                std::lock_guard lock(reg->callback_mutex);
                if (reg->callback_cancelled)
                {
                    return; // The callback was staged before its hook was removed.
                }
                ++reg->active_callbacks;
            }
            const auto* previous = active_host_view_callback_;
            active_host_view_callback_ = reg.get();
            const auto complete = utils::finally([reg, previous] {
                active_host_view_callback_ = previous;
                {
                    std::lock_guard lock(reg->callback_mutex);
                    --reg->active_callbacks;
                }
                reg->callback_finished.notify_all();
            });
            callback();
        }

        void cancel_host_view_callback(const std::shared_ptr<hook_registration>& reg, const bool may_wait)
        {
            std::unique_lock lock(reg->callback_mutex);
            reg->callback_cancelled = true;
            // A host callback may delete itself or another hook. A guest vCPU may hold the BEL.
            // Neither caller may wait for a callback that could need that same execution context.
            if (may_wait && active_host_view_callback_ == nullptr && !this->mutation_owner())
            {
                reg->callback_finished.wait(lock, [&] { return reg->active_callbacks == 0; });
            }
        }

        std::unordered_map<emulator_hook*, std::shared_ptr<hook_registration>> registrations_{};

        struct host_view_hook_range
        {
            const hook_registration* owner{};
            uint64_t address{};
            size_t size{};
            bool is_read{};
        };

        static constexpr size_t host_view_callback_limit = 512 * 1024 * 1024;

        icicle_emulator* emu_{};
        icicle_emulator* host_view_{};
        std::unique_ptr<icicle_vcpu> host_view_cpu_{};
        mutable std::mutex host_view_mutex_{};
        std::vector<std::function<void()>> host_view_callbacks_{};
        std::exception_ptr host_view_callback_error_{};
        size_t host_view_callback_bytes_{};
        std::vector<host_view_hook_range> host_view_hook_ranges_{};
        std::vector<std::pair<uint64_t, size_t>> mmio_ranges_{};
        std::vector<std::unique_ptr<icicle_vcpu>> vcpus_{};
        const bool force_smp_memory_ = [] {
            const char* value = std::getenv("SOGEN_ICICLE_FORCE_SMP_MEMORY");
            return value && std::strcmp(value, "1") == 0;
        }();
        bool exec_write_wake_enabled_ = false;
        const bool jit_profile_enabled_ = [] {
            const char* value = std::getenv("SOGEN_ICICLE_JIT_PROFILE");
            const char* origin = std::getenv("SOGEN_ICICLE_COMPILE_ORIGIN_PROFILE");
            return (value && std::strcmp(value, "1") == 0) || (origin && std::strcmp(origin, "1") == 0);
        }();
        const bool profile_enabled_ = [] {
            const char* value = std::getenv("SOGEN_SMP_PROFILE");
            return value && std::strcmp(value, "1") == 0;
        }();
        uint32_t index_{0};

        // icicle_vcpu::start() runs pending hook installs/deletes through the machine (hook machinery is
        // machine-scoped); each vCPU surfaces its own deferred hook exceptions.
        friend class icicle_vcpu;

        std::unordered_set<emulator_hook*> hooks_to_delete_{};
        std::unordered_map<emulator_hook*, memory_access_hook> hooks_to_install_{};
        // 6.5: exact/ranged execution hooks deferred from inside a running hook (defer_exec_hook_install).
        struct pending_exec_hook
        {
            uint64_t address{};
            uint64_t size{};
            memory_execution_hook_callback callback{};
        };
        std::unordered_map<emulator_hook*, pending_exec_hook> exec_hooks_to_install_{};

        // Step 6.1 (mirrors WHP whp_x86_64_emulator::partition_mutex_): guards the machine's shared hook
        // tables (registrations_/hooks_to_install_/hooks_to_delete_/index_) so the N vCPU worker threads
        // can mutate/consult them without racing. unique_lock to mutate, shared_lock to read (reads added
        // with the memory-routing work in 6.4). Lock order matches WHP: BEL -> partition_mutex, never the
        // reverse. Held only for C++ table work — never while invoking a guest/user callback.
        mutable std::shared_mutex partition_mutex_{};

        // Serializes stop-the-world pauses (see pause_peers_and); separate from partition_mutex_ so the
        // lock order is always pause_mutex_ -> partition_mutex_ and vCPU threads (which never pause)
        // can't participate in a pause-related cycle.
        std::mutex pause_mutex_{};

        // SMP 6.6c': the windows_emulator's violation callback (guest AV dispatch) for host-write perm races.
        memory_violation_hook_callback violation_callback_{};
        std::atomic_bool ever_ran_{false};
        std::mutex write_faults_mutex_{};
        std::map<size_t, std::vector<std::pair<uint64_t, size_t>>> pending_write_faults_{};

        // Step 6.2/6.3 — stop-the-world quiesce (mirrors WHP's cancel-without-stop_requested_ resume): an
        // icicle VM can only be touched by its own thread, so before a cross-VM MMU mutation we pause every
        // OTHER vCPU (icicle_stop, marking it to resume not to stop) and wait for its run_active_ to clear.
        mutable std::mutex quiesce_mutex_{};
        std::condition_variable quiesce_cv_{};
        bool quiescing_{false};
        std::vector<uint8_t> quiesce_cancel_{}; // per-vCPU: was cancelled for a mutation (resume, don't stop)
        std::vector<uint8_t> quantum_kick_{};   // per-vCPU: peer queued ops for it; end quantum early to drain

        // Step 6.5 — async cross-VM mutation. A vCPU mutating guest RAM from inside its own hook holds the
        // BEL, so it cannot pause a peer parked in a hook blocked on that BEL (deadlock, confirmed by the
        // N>1 sample probe). Instead it applies to its OWN VM now and queues the op per-peer; each peer
        // drains its queue on its own thread at begin_run_quantum, before it next executes.
        mutable std::mutex pending_mutex_{};
        // 6.6c'' per-issuer op queues: every queued op carries (issuer, seq). Same-issuer
        // sequences (the loader's map->protect) ALWAYS apply in order; the perm_epoch stale-skip
        // only applies to CROSS-issuer ops (a stale protect from another thread over a range this
        // issuer remapped). This reconciles eager application with the epoch guard.
        struct pending_op
        {
            uint64_t seq{}; // issuer-local sequence number
            uint64_t ticket{};
            std::function<void(icicle_emulator*)> apply{};
            std::optional<std::pair<uint64_t, size_t>> unmap_range{};
            // Page span is diagnostic metadata; it never changes queue order or execution.
            std::optional<std::pair<uint64_t, uint64_t>> invalidate_page_span{};
        };

        std::vector<std::vector<pending_op>> pending_ops_{}; // [vm index] -> ops(handle)
        // Next sequence number per issuer vCPU index (monotonic).
        std::vector<std::atomic<uint64_t>> issuer_seq_{};
        // A visibility mark passes only after every earlier queued op has finished applying.
        // Drains remove ops from pending_ops_ before applying them, and different workers can
        // finish their queues out of global ticket order.
        std::atomic<uint64_t> ops_issued_watermark_{0};
        uint64_t ops_completed_watermark_{0};
        std::deque<uint8_t> ops_completed_out_of_order_{};

        // 6.6: nonzero while ANY vCPU is between applying a cross-VM mutation to its own VM and
        // queueing it for peers. A peer faulting UNMAPPED with an empty queue can briefly wait for
        // the in-flight op to land (guest synchronization assumes maps are instantly coherent).
        std::atomic<uint64_t> ops_in_flight_{0};

        // Queue `op` for peer vCPU `target`, stamped with the ISSUING vCPU's sequence. External
        // (setup) callers issue as vCPU 0 semantics with their own counter — use size_t max issuer?
        // Simpler: external callers pass issuer = their own index sentinel via issuer_next().
        uint64_t issuer_next(const size_t issuer)
        {
            assert(issuer < this->issuer_seq_.size());
            return this->issuer_seq_[issuer].fetch_add(1, std::memory_order_acq_rel);
        }

        // RAII: marks a cross-VM fanout in flight (own-VM apply ... peer queueing window).
        struct fanout_guard
        {
            std::atomic<uint64_t>& counter;
            ~fanout_guard()
            {
                counter.fetch_sub(1, std::memory_order_release);
            }
        };

        void queue_op(const size_t target, const uint64_t seq, std::function<void(icicle_emulator*)> op,
                      const std::optional<std::pair<uint64_t, size_t>> unmap_range = std::nullopt,
                      const std::optional<std::pair<uint64_t, uint64_t>> invalidate_page_span = std::nullopt)
        {
            auto* self = this->mutation_owner();
            auto* profile = this->profile_enabled_ && self ? &self->smp_profile_ : nullptr;
            auto* target_profile = this->profile_enabled_ && invalidate_page_span && target < this->vcpus_.size()
                                       ? &this->vcpus_[target]->smp_profile_ : nullptr;
            const smp_profile_timer queue_timer{profile ? &profile->queue_nanos : nullptr};
            const smp_profile_timer invalidate_queue_timer{
                target_profile ? &target_profile->invalidate_queue_nanos : nullptr};
            {
                std::lock_guard<std::mutex> lock(this->pending_mutex_);
                if (target >= this->pending_ops_.size())
                {
                    return;
                }
                auto& pending = this->pending_ops_[target];
                if (target_profile)
                {
                    target_profile->invalidate_queued.fetch_add(1, std::memory_order_relaxed);
                    // An exact adjacent page-span match is a conservative duplicate candidate.
                    // Keep every op and ticket until an ordering-preserving optimization is proven.
                    if (!pending.empty() && pending.back().invalidate_page_span == invalidate_page_span)
                    {
                        target_profile->invalidate_adjacent_same_pages.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                const auto ticket = this->ops_issued_watermark_.load(std::memory_order_relaxed) + 1;
                pending.push_back(pending_op{seq, ticket, std::move(op), unmap_range, invalidate_page_span});
                this->ops_completed_out_of_order_.push_back(0);
                this->ops_issued_watermark_.store(ticket, std::memory_order_release);
            }
            if (profile)
            {
                profile->queue_ops.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void queue_invalidation(const size_t target, const uint64_t seq, const uint64_t address, const size_t size)
        {
            auto* profile = this->profile_enabled_ ? &this->vcpus_[target]->smp_profile_ : nullptr;
            std::optional<std::pair<uint64_t, uint64_t>> page_span;
            if (size)
            {
                constexpr uint64_t page_mask = ~uint64_t{0xfff};
                const auto last = address + std::min<uint64_t>(
                    static_cast<uint64_t>(size - 1), std::numeric_limits<uint64_t>::max() - address);
                page_span = std::pair{address & page_mask, last & page_mask};
            }
            this->queue_op(target, seq, [address, size, profile](icicle_emulator* h) {
                const smp_profile_timer apply_timer{profile ? &profile->invalidate_apply_nanos : nullptr};
                const bool changed = icicle_invalidate_code_range(h, address, size) != 0;
                if (profile)
                {
                    profile->invalidate_applied.fetch_add(1, std::memory_order_relaxed);
                    if (!changed)
                    {
                        profile->invalidate_no_change.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }, std::nullopt, page_span);
        }

        void complete_op(const uint64_t ticket)
        {
            std::lock_guard<std::mutex> lock(this->pending_mutex_);
            this->ops_completed_out_of_order_[ticket - this->ops_completed_watermark_ - 1] = 1;
            while (!this->ops_completed_out_of_order_.empty() && this->ops_completed_out_of_order_.front())
            {
                this->ops_completed_out_of_order_.pop_front();
                ++this->ops_completed_watermark_;
            }
        }

        static bool ranges_overlap(const uint64_t left, const size_t left_size, const uint64_t right, const size_t right_size)
        {
            if (!left_size || !right_size)
            {
                return false;
            }
            return left <= right ? right - left < left_size : left - right < right_size;
        }

        void drain_pending_unmap_prefix_before_map(icicle_vcpu& v, const uint64_t address, const size_t size)
        {
            std::vector<pending_op> prefix;
            {
                std::lock_guard<std::mutex> lock(this->pending_mutex_);
                auto& queue = this->pending_ops_[v.index()];
                const auto last_unmap = std::find_if(queue.rbegin(), queue.rend(), [&](const pending_op& op) {
                    return op.unmap_range && ranges_overlap(address, size, op.unmap_range->first, op.unmap_range->second);
                });
                if (last_unmap == queue.rend())
                {
                    return;
                }
                const auto end = last_unmap.base();
                prefix.reserve(static_cast<size_t>(end - queue.begin()));
                prefix.insert(prefix.end(), std::make_move_iterator(queue.begin()), std::make_move_iterator(end));
                queue.erase(queue.begin(), end);
            }

            t_draining_own_queue = true;
            const auto clear = utils::finally([] { t_draining_own_queue = false; });
            size_t next = 0;
            try
            {
                for (; next < prefix.size(); ++next)
                {
                    prefix[next].apply(v.handle());
                    this->complete_op(prefix[next].ticket);
                }
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(this->pending_mutex_);
                auto& queue = this->pending_ops_[v.index()];
                queue.insert(queue.begin(), std::make_move_iterator(prefix.begin() + next), std::make_move_iterator(prefix.end()));
                throw;
            }
        }

        emulator_hook* fresh_hook_handle()
        {
            std::unique_lock lock(this->partition_mutex_);
            const auto id = ++this->index_;
            return reinterpret_cast<emulator_hook*>(static_cast<size_t>(id));
        }

        // Serializes stop-the-world operations (guards the quiesce handshake's single quiescing_ flag).
        // Deliberately separate from partition_mutex_ so hook-table work can lock partition_mutex_ inside a
        // paused section without recursive locking. Lock order: pause_mutex_ -> partition_mutex_, never the
        // reverse, and vCPU threads never pause at all (route_to_all_vms).
        template <typename Fn>
        void pause_peers_and(Fn&& fn)
        {
            if (this->vcpus_.size() == 1)
            {
                fn();
                return;
            }
            assert(this->mutation_owner() == nullptr); // only external callers may wait while paused
            std::lock_guard<std::mutex> plock(this->pause_mutex_);
            this->run_with_vcpus_paused(std::forward<Fn>(fn));
        }

        // 6.5 routing rule: a mutation requested by a running vCPU or an explicitly bound
        // scheduler worker (including host work between quanta that may hold the BEL) applies to
        // its OWN VM now and queues the op for each peer, drained on the peer's thread at
        // begin_run_quantum. An external/setup caller pauses peers instead — it holds no BEL, so a peer
        // parked in a hook can always acquire the BEL, finish, exit run(), and become stoppable. Because
        // vCPU threads never pause, the A-B/B-A hazard between partition_mutex_ and the quiesce wait
        // cannot form.
        void route_to_all_vms(const std::function<void(size_t)>& per_vm_op,
                              const std::optional<std::pair<uint64_t, size_t>> unmap_range = std::nullopt)
        {
            if (this->vcpus_.size() == 1)
            {
                per_vm_op(0);
                return;
            }
            auto* self = this->mutation_owner();
            const bool caller_is_vcpu = self != nullptr;
            if (!caller_is_vcpu)
            {
                this->pause_peers_and([&] {
                    for (size_t i = 0; i < this->vcpus_.size(); ++i)
                    {
                        per_vm_op(i);
                    }
                });
                return;
            }
            per_vm_op(self->index());
            const auto own = self->index();
            const uint64_t rseq = this->issuer_next(own);
            for (auto& v : this->vcpus_)
            {
                if (v->index() != own)
                {
                    const auto i = v->index();
                    this->queue_op(i, rseq, [this, i, per_vm_op](icicle_emulator*) { per_vm_op(i); }, unmap_range);
                }
            }
            // Bounded application latency: a peer mid-quantum must not keep executing (potentially
            // faulting on just-changed mappings) for its whole quantum before the op drains.
            this->kick_peers(own);
        }

        emulator_hook* hook_memory_access(memory_access_hook hook, emulator_hook* hook_id)
        {
            auto* handle = hook_id ? hook_id : this->fresh_hook_handle();
            auto reg = std::make_shared<hook_registration>();
            {
                std::unique_lock lock(this->partition_mutex_);
                this->registrations_[handle] = reg;
            }

            this->route_to_all_vms([this, hook, reg](const size_t i) {
                if (reg->deleted)
                {
                    return; // deleted before this VM's install drained
                }
                auto* const vm = this->vcpus_[i]->handle();
                uint32_t id{};
                std::unique_ptr<utils::object> object;
                if (hook.observation)
                {
                    // Shape + bind the observation to vCPU i (outcome + host/guest origin), deferring to it.
                    auto shaped = [this, i, cb = hook.observation](uint64_t access, const void* data, size_t length,
                                                                   uint64_t error, int32_t host_write) {
                        try
                        {
                            cb(this->acting_cpu(i), access, data, length,
                               {.outcome = error == 0 ? memory_access_outcome::completed : memory_access_outcome::failed,
                                .backend_error = error,
                                .origin = host_write != 0 ? memory_write_origin::host : memory_write_origin::guest});
                        }
                        catch (...)
                        {
                            this->acting_sink(i)->defer_hook_exception(std::current_exception());
                        }
                    };
                    auto obj = make_function_object(
                        std::function<void(uint64_t, const void*, size_t, uint64_t, int32_t)>(std::move(shaped)));
                    auto* ptr = obj.get();
                    auto* wrapper = +[](void* user, uint64_t address, const void* data, size_t length, uint64_t error, int32_t host_write) {
                        (*static_cast<decltype(ptr)>(user))(address, data, length, error, host_write);
                    };
                    id = icicle_add_write_observation_hook(vm, hook.address, hook.address + hook.size, wrapper, ptr);
                    object = std::move(obj);
                }
                else
                {
                    auto obj = make_function_object(this->bind_cpu(i, hook.callback));
                    auto* ptr = obj.get();
                    auto* wrapper = +[](void* user, const uint64_t address, const void* data, size_t length) {
                        const auto& func = *static_cast<decltype(ptr)>(user);
                        func(address, data, length);
                    };

                    auto* installer = hook.is_read ? &icicle_add_read_hook : &icicle_add_write_hook;
                    id = installer(vm, hook.address, hook.address + hook.size, wrapper, ptr);
                    object = std::move(obj);
                }
                if (id == 0)
                {
                    throw std::runtime_error("Icicle memory hook registration failed");
                }
                std::unique_lock lock(this->partition_mutex_);
                reg->entries.emplace_back(i, id, std::move(object));
            });

            if (this->host_view_)
            {
                this->install_host_view_memory_hook(hook, reg);
            }
            return handle;
        }

        void install_host_view_memory_hook(const memory_access_hook& hook, const std::shared_ptr<hook_registration>& reg)
        {
            std::lock_guard view_lock(this->host_view_mutex_);
            uint32_t id = 0;
            std::unique_ptr<utils::object> object;
            if (hook.observation)
            {
                auto shaped = [this, weak_reg = std::weak_ptr<hook_registration>{reg}, cb = hook.observation](
                                  const uint64_t address, const void* data, const size_t length,
                                  const uint64_t error, const int32_t host_write) {
                    try
                    {
                        if (length > host_view_callback_limit - this->host_view_callback_bytes_)
                        {
                            throw std::length_error("Icicle host view hook data exceeds staging limit");
                        }
                        std::vector<std::byte> bytes(length);
                        std::memcpy(bytes.data(), data, length);
                        auto callback_reg = weak_reg.lock();
                        if (!callback_reg)
                        {
                            return;
                        }
                        this->host_view_callbacks_.emplace_back(
                            [this, reg = std::move(callback_reg), cb, address, bytes = std::move(bytes), error, host_write] {
                                this->invoke_host_view_callback(reg, [&] {
                                    cb(*this->host_view_cpu_, address, bytes.data(), bytes.size(),
                                       {.outcome = error == 0 ? memory_access_outcome::completed : memory_access_outcome::failed,
                                        .backend_error = error,
                                        .origin = host_write != 0 ? memory_write_origin::host : memory_write_origin::guest});
                                });
                            });
                        this->host_view_callback_bytes_ += length;
                    }
                    catch (...)
                    {
                        if (!this->host_view_callback_error_)
                        {
                            this->host_view_callback_error_ = std::current_exception();
                        }
                    }
                };
                auto obj = make_function_object(std::function<void(uint64_t, const void*, size_t, uint64_t, int32_t)>(std::move(shaped)));
                auto* ptr = obj.get();
                auto* wrapper = +[](void* user, uint64_t address, const void* data, size_t length, uint64_t error, int32_t host_write) {
                    (*static_cast<decltype(ptr)>(user))(address, data, length, error, host_write);
                };
                id = icicle_add_write_observation_hook(this->host_view_, hook.address, hook.address + hook.size, wrapper, ptr);
                object = std::move(obj);
            }
            else
            {
                auto shaped = [this, weak_reg = std::weak_ptr<hook_registration>{reg}, cb = hook.callback](
                                  const uint64_t address, const void* data, const size_t length) {
                    try
                    {
                        if (length > host_view_callback_limit - this->host_view_callback_bytes_)
                        {
                            throw std::length_error("Icicle host view hook data exceeds staging limit");
                        }
                        std::vector<std::byte> bytes(length);
                        std::memcpy(bytes.data(), data, length);
                        auto callback_reg = weak_reg.lock();
                        if (!callback_reg)
                        {
                            return;
                        }
                        this->host_view_callbacks_.emplace_back(
                            [this, reg = std::move(callback_reg), cb, address, bytes = std::move(bytes)] {
                                this->invoke_host_view_callback(reg, [&] {
                                    cb(*this->host_view_cpu_, address, bytes.data(), bytes.size());
                                });
                            });
                        this->host_view_callback_bytes_ += length;
                    }
                    catch (...)
                    {
                        if (!this->host_view_callback_error_)
                        {
                            this->host_view_callback_error_ = std::current_exception();
                        }
                    }
                };
                auto obj = make_function_object(std::function<void(uint64_t, const void*, size_t)>(std::move(shaped)));
                auto* ptr = obj.get();
                auto* wrapper = +[](void* user, const uint64_t address, const void* data, const size_t length) {
                    (*static_cast<decltype(ptr)>(user))(address, data, length);
                };
                auto* installer = hook.is_read ? &icicle_add_read_hook : &icicle_add_write_hook;
                id = installer(this->host_view_, hook.address, hook.address + hook.size, wrapper, ptr);
                object = std::move(obj);
            }
            if (id == 0)
            {
                throw std::runtime_error("Icicle host view memory hook registration failed");
            }
            std::unique_lock lock(this->partition_mutex_);
            if (reg->deleted)
            {
                icicle_remove_hook(this->host_view_, id);
            }
            else
            {
                reg->entries.emplace_back(this->vcpus_.size(), id, std::move(object));
                this->host_view_hook_ranges_.push_back({reg.get(), hook.address, static_cast<size_t>(hook.size), hook.is_read});
            }
        }

        void delete_hook_internal(emulator_hook* hook)
        {
            std::shared_ptr<hook_registration> reg;
            {
                std::unique_lock lock(this->partition_mutex_);
                auto it = this->registrations_.find(hook);
                if (it == this->registrations_.end())
                {
                    return;
                }
                reg = it->second;
                if (reg->pending)
                {
                    // Reserved but not yet installed (queued from inside a running hook): cancel the install.
                    this->hooks_to_install_.erase(hook);
                    this->exec_hooks_to_install_.erase(hook);
                    this->registrations_.erase(it);
                    return;
                }
                reg->deleted = true; // in-flight peer installs must not resurrect this hook
                this->registrations_.erase(it);
            }
            this->cancel_host_view_callback(reg, true);

            this->route_to_all_vms([this, reg](const size_t i) {
                std::vector<uint32_t> ids;
                {
                    std::unique_lock lock(this->partition_mutex_);
                    for (auto it = reg->entries.begin(); it != reg->entries.end();)
                    {
                        if (std::get<0>(*it) == i)
                        {
                            ids.push_back(std::get<1>(*it));
                            it = reg->entries.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }
                // Runs on VM i's own thread (async drain) or under the pause (external caller): touching
                // this VM's handle is safe in both.
                for (const auto id : ids)
                {
                    icicle_remove_hook(this->vcpus_[i]->handle(), id);
                }
            });
            if (this->host_view_)
            {
                std::lock_guard view_lock(this->host_view_mutex_);
                std::vector<uint32_t> ids;
                {
                    std::unique_lock lock(this->partition_mutex_);
                    for (auto it = reg->entries.begin(); it != reg->entries.end();)
                    {
                        if (std::get<0>(*it) == this->vcpus_.size())
                        {
                            ids.push_back(std::get<1>(*it));
                            it = reg->entries.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }
                for (const auto id : ids)
                {
                    icicle_remove_hook(this->host_view_, id);
                }
                std::erase_if(this->host_view_hook_ranges_,
                              [owner = reg.get()](const host_view_hook_range& range) { return range.owner == owner; });
            }
        }

        // Drains deferred memory-hook installs (read/write/observation). Safe from the
        // run_on_next_instruction one-shot: read/write hooks live in separate RefCells from the
        // execution_hooks the one-shot fires under.
        void drain_memory_hook_installs()
        {
            std::vector<std::pair<emulator_hook*, memory_access_hook>> installs;
            {
                std::unique_lock lock(this->partition_mutex_);
                for (auto& [k, v] : this->hooks_to_install_)
                {
                    installs.emplace_back(k, std::move(v));
                }
                this->hooks_to_install_.clear();
            }
            for (auto& [handle, hook] : installs)
            {
                this->hook_memory_access(std::move(hook), handle);
            }
        }

        // Drains deferred execution-hook installs. Must run OUTSIDE icicle run() (execution_hooks is
        // borrowed while execution hooks fire): begin_run_quantum or quantum end — never the one-shot.
        void drain_exec_hook_installs()
        {
            std::vector<std::pair<emulator_hook*, pending_exec_hook>> installs;
            {
                std::unique_lock lock(this->partition_mutex_);
                for (auto& [k, v] : this->exec_hooks_to_install_)
                {
                    installs.emplace_back(k, std::move(v));
                }
                this->exec_hooks_to_install_.clear();
            }
            for (auto& [handle, pending] : installs)
            {
                this->install_exec_hook(pending.address, pending.size, pending.callback, handle);
            }
        }

        void drain_deferred_deletes()
        {
            std::vector<emulator_hook*> deletes;
            {
                std::unique_lock lock(this->partition_mutex_);
                deletes.assign(this->hooks_to_delete_.begin(), this->hooks_to_delete_.end());
                this->hooks_to_delete_.clear();
            }
            for (auto* hook : deletes)
            {
                this->delete_hook_internal(hook);
            }
        }

        // Full drain at the end of start() (outside run() — safe for every hook type).
        void perform_pending_actions()
        {
            // Runs at the end of every vCPU's start() on its worker thread → concurrent across vCPUs.
            this->drain_memory_hook_installs();
            this->drain_exec_hook_installs();
            this->drain_deferred_deletes();
        }

        // Entry point for the deferred-action one-shot (run_on_next_instruction): MEMORY hooks only —
        // execution hooks must not be installed from inside the one-shot (execution_hooks RefCell).
        void perform_pending_hook_installs()
        {
            this->drain_memory_hook_installs();
        }

        emulator_hook* try_install_memory_access_hook(memory_access_hook hook)
        {
            if (hook.size == 0 || hook.size > std::numeric_limits<uint64_t>::max() - hook.address)
            {
                throw std::invalid_argument("Invalid Icicle memory hook range");
            }
            if (!detail::in_hook_flag())
            {
                return this->hook_memory_access(std::move(hook), nullptr);
            }

            auto* hook_id = this->fresh_hook_handle();
            {
                std::unique_lock lock(this->partition_mutex_);
                auto reg = std::make_shared<hook_registration>();
                reg->pending = true;
                this->registrations_[hook_id] = std::move(reg);
                this->hooks_to_install_[hook_id] = std::move(hook);
            }

            this->schedule_action_execution();

            return hook_id;
        }

        void schedule_action_execution()
        {
            this->run_on_next_instruction([this] {
                this->perform_pending_hook_installs(); //
            });
        }

        void run_on_next_instruction(std::function<void()> func)
        {
            // Deferred actions run inside icicle's Rust frames too: report failures instead of
            // silently ignoring them, but never let them unwind across the boundary.
            auto* heap_func = new std::function<void()>([this, action = std::move(func)] {
                try
                {
                    action();
                }
                catch (...)
                {
                    this->defer_hook_exception(std::current_exception());
                }
            });
            auto* callback = +[](void* data) {
                auto* cb = static_cast<std::function<void()>*>(data);

                const hook_scope exec_scope(&detail::in_execution_hook_flag()); // scheduled into execution_hooks
                try
                {
                    (*cb)();
                }
                catch (...)
                {
                    // Last resort: defer_hook_exception itself is noexcept, so nothing should reach here.
                }

                delete cb;
            };

            // 6.5: target the ACTING vCPU's VM — the deferral came from that vCPU's hook, so its next
            // instruction is imminent (the originating syscall has not even returned yet). Falls back to
            // the master VM for external callers and at N=1.
            icicle_run_on_next_instruction(this->acting_handle(), callback, heap_func);
        }

        // 6.2 exec_start gate: a vCPU marks itself running here. It must not enter icicle run() while a
        // stop-the-world is in progress (else its VM would be touched cross-thread), so it waits for
        // quiescing_ to clear; setting run_active_ under the same lock that guards quiescing_ closes the
        // parked->executing race with run_with_vcpus_paused. N=1: no peers, so no gate.
        void begin_run_quantum(icicle_vcpu& v)
        {
            this->ever_ran_.store(true);
            if (this->vcpus_.size() == 1)
            {
                v.run_active_ = true;
                this->drain_exec_hook_installs(); // self-kicked deferred installs drain outside run()
                icicle_reconcile_exec_write_wake(v.handle());
                return;
            }
            {
                std::unique_lock lock(this->quiesce_mutex_);
                this->quiesce_cv_.wait(lock, [this] { return !this->quiescing_; });
                v.run_active_ = true;
            }
            // 6.5: drain AFTER run_active_ is set. An external stop-the-world now observes this vCPU as
            // running and stops/waits for it, so a pauser can never touch our VM while we are applying
            // queued ops to it (the old drain-before-mark order left that cross-thread window open).
            this->drain_pending_ops(v);
            this->drain_exec_hook_installs(); // outside run(): execution_hooks is not borrowed here
            this->dispatch_deferred_write_faults(v);
            icicle_reconcile_exec_write_wake(v.handle());
        }

        // 6.6c'': surface between-quantum host-write perm failures as guest AVs at a REAL vCPU
        // context (own thread, outside any syscall handler) - the safe dispatch point.
        void dispatch_deferred_write_faults(icicle_vcpu& v)
        {
            if (!this->violation_callback_)
            {
                return;
            }
            std::vector<std::pair<uint64_t, size_t>> faults;
            {
                std::lock_guard<std::mutex> lock(this->write_faults_mutex_);
                faults.swap(this->pending_write_faults_[v.index()]);
            }
            for (auto& fault : faults)
            {
                try
                {
                    (void)this->violation_callback_(this->acting_cpu(v.index()), fault.first, fault.second,
                                                    memory_operation::write, memory_violation_type::protection);
                }
                catch (...)
                {
                    this->acting_sink(v.index())->defer_hook_exception(std::current_exception());
                }
            }
        }

        // 6.5 — run the ops queued for this vCPU on its own thread (safe), before it next executes.
        void drain_pending_ops(icicle_vcpu& v)
        {
            std::vector<pending_op> ops;
            {
                std::lock_guard<std::mutex> lock(this->pending_mutex_);
                ops.swap(this->pending_ops_[v.index_]);
            }
            // 6.6c'': reentrancy guard - drained ops (maps/protects) can re-enter host write paths;
            // a nested drain must not run (double-apply/ordering corruption + the CrossVmMap/
            // RangedExecHook regressions). Ops queued WHILE draining simply wait for the next drain.
            if (t_draining_own_queue)
            {
                // Put them back in order (front = oldest) so nothing is lost.
                std::lock_guard<std::mutex> lock(this->pending_mutex_);
                auto& queue = this->pending_ops_[v.index_];
                queue.insert(queue.begin(), std::make_move_iterator(ops.begin()), std::make_move_iterator(ops.end()));
                return;
            }
            t_draining_own_queue = true;
            const auto clear = utils::finally([] { t_draining_own_queue = false; });
            for (auto& op : ops)
            {
                op.apply(v.handle());
                this->complete_op(op.ticket);
            }
        }

        // 6.6: drain this vCPU's own queue; if empty while a cross-VM fanout is in flight,
        // bounded-wait for the in-flight op to land first (drain+retry's shared core).
        void drain_own_queue_with_inflight_wait(icicle_vcpu& v)
        {
            if (t_draining_own_queue)
            {
                return;
            }
            t_draining_own_queue = true;
            const auto clear = utils::finally([] { t_draining_own_queue = false; });
            std::vector<pending_op> ops;
            {
                std::lock_guard<std::mutex> lock(this->pending_mutex_);
                ops.swap(this->pending_ops_[v.index()]);
            }
            if (ops.empty() && this->ops_in_flight_.load(std::memory_order_acquire) > 0)
            {
                for (int spin = 0; spin < 4000 && ops.empty(); ++spin)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    std::lock_guard<std::mutex> lock(this->pending_mutex_);
                    ops.swap(this->pending_ops_[v.index()]);
                }
            }
            for (auto& op : ops)
            {
                op.apply(v.handle());
                this->complete_op(op.ticket);
            }
        }

        // 6.6c'': apply ONE queued op (pure C-ABI; issuer ordering already encoded in the
        // queue's FIFO). Returns false when empty.
        bool apply_one_pending_op(icicle_vcpu& v)
        {
            pending_op op;
            {
                std::lock_guard<std::mutex> lock(this->pending_mutex_);
                auto& queue = this->pending_ops_[v.index_];
                if (queue.empty())
                {
                    return false;
                }
                op = std::move(queue.front());
                queue.erase(queue.begin());
            }
            op.apply(v.handle());
            this->complete_op(op.ticket);
            return true;
        }

        // 6.5 — apply a SELF-CONTAINED `op(handle)` (touches only the given handle, never reads another VM)
        // to every vCPU VM safely, via the 6.5 routing rule (see route_to_all_vms): a vCPU caller applies
        // to its own VM now, queues peers, and kicks them for bounded drain latency; an external/setup
        // caller (or N=1) pauses peers and applies to all directly.
        void apply_to_all_vms(const std::function<void(icicle_emulator*)>& op)
        {
            // `op` MUST be captured by value: the lambda is QUEUED for peers and invoked on their
            // threads at begin_run_quantum — long after this call returned. A reference capture dangles
            // and the peer's drain invokes a destroyed std::function (std::bad_function_call — caught by
            // the N>1 sample probe under cdb: drain_pending_ops -> queued route op).
            this->route_to_all_vms([this, op](const size_t i) { op(this->vcpus_[i]->handle()); });
        }

        // 6.2 — a vCPU calls this right after icicle run() returns: clears run_active_, wakes any
        // stop-the-world waiter, and (if this vCPU was cancelled for a peer's cross-VM mutation, not a real
        // stop or a hook exception) waits for that mutation to finish and reports that the quantum should
        // resume (mirrors WHP's "cancel without stop_requested_ -> continue").
        // 6.5: end THIS vCPU's quantum at the next block boundary so begin_run_quantum can drain deferred
        // work outside icicle run() (same-thread stop: just sets the stop flag + zeroes the icount limit).
        void kick_self(const size_t index)
        {
            {
                std::lock_guard<std::mutex> lock(this->quiesce_mutex_);
                this->quantum_kick_[index] = 1;
            }
            icicle_stop(this->vcpus_[index]->handle());
        }

        void abort_run_quantum(icicle_vcpu& v)
        {
            if (this->vcpus_.size() == 1)
            {
                v.run_active_ = false;
                return;
            }
            std::lock_guard<std::mutex> lock(this->quiesce_mutex_);
            v.run_active_ = false;
            this->quiesce_cv_.notify_all();
        }

        bool complete_run_quantum(icicle_vcpu& v)
        {
            if (this->vcpus_.size() == 1)
            {
                v.run_active_ = false;
                // N=1: honor a self-kick (deferred execution-hook install) by resuming the quantum —
                // begin_run_quantum drains it outside icicle run().
                std::lock_guard<std::mutex> lock(this->quiesce_mutex_);
                if (this->quantum_kick_[v.index_])
                {
                    this->quantum_kick_[v.index_] = 0;
                    return true;
                }
                if (!v.stop_requested_ && !v.pending_hook_exception_ && icicle_exec_write_pending(v.handle()))
                {
                    return true;
                }
                return false;
            }
            // Read VM state while run_active_ still excludes an external pauser.
            // start() reacquires the parked gate after the completion handoff.
            const bool exec_write_pending = icicle_exec_write_pending(v.handle());
            std::unique_lock lock(this->quiesce_mutex_);
            // Retire the active claim under the same mutex the external pauser
            // uses to test it. All VM reads above are complete; start() takes
            // the parked gate before its next VM read.
            v.run_active_ = false;
            this->quiesce_cv_.notify_all();
            if (v.stop_requested_ || v.pending_hook_exception_)
            {
                return false;
            }
            if (this->quiesce_cancel_[v.index_])
            {
                this->quiesce_cancel_[v.index_] = 0;
                this->quiesce_cv_.wait(lock, [this] { return !this->quiescing_; });
                return true;
            }
            if (this->quantum_kick_[v.index_])
            {
                // A peer queued a cross-VM op for this vCPU and kicked us (kick_peers): end this quantum
                // early so begin_run_quantum drains the queue before any further guest execution. Same
                // resume mechanics as a quiesce cancel, minus the wait (no stop-the-world holds us).
                this->quantum_kick_[v.index_] = 0;
                return true;
            }
            return !v.stop_requested_ && !v.pending_hook_exception_ && exec_write_pending;
        }

        // 6.5: after a vCPU queues cross-VM ops for peers (async map/protect/unmap), kick each RUNNING
        // peer so its current quantum ends promptly (icicle checks the stop flag at block boundaries) and
        // begin_run_quantum drains the queue. Without the kick a spinning peer could run its whole quantum
        // — faulting on a mapping whose pointer it can already observe — before draining. Residual race
        // (peer's last few blocks before the kick lands) is the documented 6.6 TLB-coherency window.
        void kick_peers(const size_t own_index)
        {
            auto* self = this->mutation_owner();
            auto* profile = this->profile_enabled_ && own_index < this->vcpus_.size() &&
                                    self == this->vcpus_[own_index].get() ? &self->smp_profile_ : nullptr;
            const smp_profile_timer kick_timer{profile ? &profile->kick_nanos : nullptr};
            if (profile)
            {
                profile->kick_calls.fetch_add(1, std::memory_order_relaxed);
            }
            std::lock_guard<std::mutex> lock(this->quiesce_mutex_);
            for (auto& v : this->vcpus_)
            {
                if (v->index_ == own_index || !v->run_active_.load())
                {
                    continue;
                }
                this->quantum_kick_[v->index_] = 1;
                icicle_stop(v->handle());
                if (profile)
                {
                    profile->kick_targets.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        // 6.3 — run `fn` with every OTHER vCPU guaranteed to be outside icicle run() (so touching its VM
        // from this thread is safe). Pauses running peers via icicle_stop (marking them to RESUME, not
        // stop), waits for each run_active_ to clear, runs fn, then releases them. The caller's own vCPU (if
        // any, on this thread) is excluded — its VM is safe to touch same-thread. N=1: no peers.
        template <typename Fn>
        void run_with_vcpus_paused(Fn&& fn)
        {
            if (this->vcpus_.size() == 1)
            {
                fn();
                return;
            }
            std::unique_lock lock(this->quiesce_mutex_);
            this->quiescing_ = true;
            const auto resume = utils::finally([this, &lock] {
                if (!lock.owns_lock())
                {
                    lock.lock();
                }
                this->quiescing_ = false;
                this->quiesce_cv_.notify_all();
            });
            for (auto& vcpu : this->vcpus_)
            {
                if (vcpu.get() == t_running_vcpu)
                {
                    continue;
                }
                if (vcpu->run_active_)
                {
                    this->quiesce_cancel_[vcpu->index()] = 1;
                    icicle_stop(vcpu->handle());
                }
            }
            // Heartbeat wait (SOGEN_SMP_TRACE): a stuck pause prints which vCPU never cleared
            // run_active_, so a hung run's stderr tail names the deadlock participant directly.
            for (int waited_s = 0;; ++waited_s)
            {
                const bool ready = [this] {
                    for (auto& vcpu : this->vcpus_)
                    {
                        if (vcpu.get() != t_running_vcpu && vcpu->run_active_)
                        {
                            return false;
                        }
                    }
                    return true;
                }();
                if (ready)
                {
                    break;
                }
                // Dedicated bounded pause probe avoids enabling the very verbose SMP trace.
                const auto* pause_probe = std::getenv("SOGEN_SMP_PAUSE_PROBE");
                if ((smp_trace_enabled() || (pause_probe && *pause_probe == '1')) &&
                    waited_s > 0 && waited_s % 2 == 0)
                {
                    std::fprintf(stderr, "[SMPTRC] pause still waiting (%ds) for: ", waited_s);
                    for (auto& vcpu : this->vcpus_)
                    {
                        if (vcpu.get() != t_running_vcpu && vcpu->run_active_)
                        {
                            std::fprintf(stderr, "vcpu%u ", vcpu->index_);
                        }
                    }
                    std::fprintf(stderr, "\n");
                }
                this->quiesce_cv_.wait_for(lock, std::chrono::seconds(1), [this] {
                    for (auto& vcpu : this->vcpus_)
                    {
                        if (vcpu.get() != t_running_vcpu && vcpu->run_active_)
                        {
                            return false;
                        }
                    }
                    return true;
                });
            }
            // Keep quiescing_ true so no worker can enter a new quantum, but release its
            // mutex before waiting for parked-owner gates. A worker may hold its gate and
            // the kernel lock while a queued operation briefly needs quiesce_mutex_.
            lock.unlock();
            std::vector<std::unique_lock<std::recursive_mutex>> parked_owners;
            parked_owners.reserve(this->vcpus_.size());
            for (auto& vcpu : this->vcpus_)
            {
                parked_owners.emplace_back(vcpu->parked_vm_mutex_);
            }
            // Every VM is now excluded from both guest execution and parked host use.
            // parked_owners is destroyed before resume clears quiescing_, including on throw.
            fn();
        }
    };

    // icicle_vcpu methods that need the complete machine type (memory surface + hook machinery).
    inline memory_interface& icicle_vcpu::memory()
    {
        return this->machine_;
    }

    inline const memory_interface& icicle_vcpu::memory() const
    {
        return this->machine_;
    }

    inline void icicle_vcpu::start(const size_t count)
    {
        if (this->host_view_only_)
        {
            throw std::logic_error("Icicle host-view callback has no running CPU context");
        }
        auto* previous_worker = std::exchange(t_worker_vcpu, static_cast<void*>(this));
        auto* previous_running = std::exchange(t_running_vcpu, static_cast<void*>(this));
        const auto clear_current = utils::finally([previous_worker, previous_running] {
            t_running_vcpu = previous_running;
            t_worker_vcpu = previous_worker;
        });
        // stop() may run after the scheduler releases its lock but before this
        // quantum marks run_active_. Preserve that pending request until we have
        // crossed the begin_run_quantum gate and can check it on the owner thread.
        // perform_thread_switch() clears a consumed stop under the kernel lock.
        // start(count) must run at most `count` instructions in TOTAL. A kick-ended quantum resumes
        // here, and re-issuing the full count would extend the budget past the caller's contract
        // (caught by RangedExecHook...: the resumed vCPU ran off the end of its code). Track the
        // remainder via the VM's retired-instruction counter. count==0 means unlimited (icicle contract).
        std::unique_lock<std::recursive_mutex> parked_access(this->parked_vm_mutex_);
        const uint64_t base_icount = icicle_get_icount(this->emu_);
        uint32_t empty_interrupt_retries = 0;
        for (;;)
        {
            // Budget check BEFORE begin_run_quantum: breaking after it would leak run_active_ = true
            // (set by begin), deadlocking the next external stop-the-world waiting on this vCPU.
            uint64_t remaining = count;
            if (count != 0)
            {
                const uint64_t executed = icicle_get_icount(this->emu_) - base_icount;
                if (executed >= count)
                {
                    break; // budget fully consumed by the interrupted quantum
                }
                remaining = count - executed;
            }
            parked_access.unlock(); // begin may wait for an external pause; never hold this gate then
            auto retire_on_error = utils::finally([this] { this->machine_.abort_run_quantum(*this); });
            this->machine_.begin_run_quantum(*this);
            if (this->stop_requested_.load(std::memory_order_acquire))
            {
                // A prestart switch or shutdown needs no guest instructions.
                // Retire run_active_ before returning so a peer waiting to
                // mutate all VMs cannot deadlock on this worker.
                (void)this->machine_.complete_run_quantum(*this);
                retire_on_error.cancel();
                parked_access.lock();
                return;
            }
            const uint64_t before_icount = icicle_get_icount(this->emu_);
            icicle_start(this->emu_, remaining);
            const bool resume_quantum = this->machine_.complete_run_quantum(*this);
            retire_on_error.cancel();
            parked_access.lock(); // protect post-run VM reads while run_active_ is false
            if (resume_quantum)
            {
                continue; // paused for a peer's cross-VM mutation — resume this quantum
            }
            // Unlimited SMP runs have no instruction budget to exhaust. A remote
            // atomic interrupt can arrive as a kick is retired between the Rust VM
            // exit and the C++ scheduler check. If neither a real stop nor a hook
            // exception is pending, resume on this vCPU instead of terminating all
            // workers. Bound zero-progress retries so a permanently asserted flag
            // is still reported as a backend failure rather than spinning forever.
            if (count == 0 && !this->stop_requested_.load(std::memory_order_acquire) &&
                !this->pending_hook_exception_)
            {
                icicle_stop_info info{};
                ice(icicle_get_stop_info(this->emu_, &info) != 0, "Failed to read icicle stop info");
                if (static_cast<icicle_stop_kind>(info.kind) == icicle_stop_kind::instruction_limit)
                {
                    empty_interrupt_retries = icicle_get_icount(this->emu_) == before_icount
                        ? empty_interrupt_retries + 1 : 0;
                    ice(empty_interrupt_retries <= 16, "Icicle repeatedly interrupted without guest progress");
                    continue;
                }
            }
            break;
        }
        if (const char* probe = std::getenv("SOGEN_STOP_PROBE"); probe && *probe == '1')
        {
            static std::atomic<uint32_t> samples{0};
            if (samples.fetch_add(1, std::memory_order_relaxed) < 4000)
            {
                icicle_stop_info info{};
                (void)icicle_get_stop_info(this->emu_, &info);
                std::string description;
                icicle_get_vm_exit_description(this->emu_,
                    [](void* data, const void* value, size_t size) {
                        static_cast<std::string*>(data)->assign(static_cast<const char*>(value), size);
                    }, &description);
                std::fprintf(stderr, "[STOPPROBE] vcpu=%u kind=%u vm=%s stop_requested=%u hook_exception=%u icount=%llu\n",
                             this->index_, info.kind, description.c_str(),
                             static_cast<unsigned>(this->stop_requested_.load(std::memory_order_acquire)),
                             static_cast<unsigned>(this->pending_hook_exception_ != nullptr),
                             static_cast<unsigned long long>(icicle_get_icount(this->emu_)));
            }
        }
        this->rethrow_deferred_hook_exception();
        this->throw_if_unhandled_stop();
        this->machine_.perform_pending_actions();
    }

    inline void icicle_vcpu::throw_if_unhandled_stop()
    {
        icicle_stop_info info{};
        ice(icicle_get_stop_info(this->emu_, &info) != 0, "Failed to read icicle stop info");

        const auto kind = static_cast<icicle_stop_kind>(info.kind);
        if (kind == icicle_stop_kind::none || kind == icicle_stop_kind::instruction_limit)
        {
            return;
        }

        std::array<char, 320> message{};
        if (kind == icicle_stop_kind::unhandled_exception)
        {
            std::string name{};
            icicle_get_exception_name(
                info.code,
                [](void* data, const void* text, const size_t length) {
                    static_cast<std::string*>(data)->assign(static_cast<const char*>(text), length);
                },
                &name);
            std::snprintf(message.data(), message.size(),
                          "Icicle stopped on unhandled exception: code=0x%X (%s) value=0x%llX rip=0x%llX", info.code, name.c_str(),
                          static_cast<unsigned long long>(info.value),
                          static_cast<unsigned long long>(this->read_instruction_pointer()));
        }
        else
        {
            std::string description{};
            icicle_get_vm_exit_description(
                this->emu_,
                [](void* data, const void* text, const size_t length) {
                    static_cast<std::string*>(data)->assign(static_cast<const char*>(text), length);
                },
                &description);
            std::snprintf(message.data(), message.size(), "Icicle stopped on unhandled VM exit: %s code=0x%X value=0x%llX rip=0x%llX",
                          description.c_str(), info.code, static_cast<unsigned long long>(info.value),
                          static_cast<unsigned long long>(this->read_instruction_pointer()));
        }

        throw std::runtime_error(message.data());
    }

    std::unique_ptr<x86_64_emulator> create_x86_64_emulator(const size_t vcpu_count)
    {
        return std::make_unique<icicle_x86_64_emulator>(vcpu_count);
    }
} // namespace sogen::icicle
