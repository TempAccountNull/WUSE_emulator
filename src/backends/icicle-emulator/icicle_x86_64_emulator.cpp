#define ICICLE_EMULATOR_IMPL
#include "icicle_x86_64_emulator.hpp"
#include "execution_hook.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <tuple>
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

    icicle_emulator* icicle_create_emulator(uint64_t memory_limit_mib);
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
    // SMP 6.6: drop this VM's translations for a range (returns 1 if anything was cached), and a
    // read-only query for whether ANY sharing VM translated the range (shared IN_CODE_CACHE perms).
    int32_t icicle_invalidate_code_range(icicle_emulator*, uint64_t address, uint64_t length);
    int32_t icicle_code_range_is_cached(icicle_emulator*, uint64_t address, uint64_t length);
    // SMP 6.6c'': smallest perm epoch over a range (0 = not shared/unmapped) for stale-op detection.
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

    // The vCPU this OS thread OWNS (set at start entry, never cleared). Between quanta -
    // after start() returns, while the windows_emulator worker still runs host code on this
    // thread - t_running_vcpu is null but the thread still belongs to this vCPU, so host-write
    // perm failures can be attributed and DEFERRED to this vCPU's next quantum (6.6c'').
    thread_local void* t_worker_vcpu = nullptr;
    // True while THIS thread is inside drain_pending_ops: a drained op (e.g. a map) can re-enter
    // try_write_memory, which would otherwise drain again (re-entrancy broke CrossVmMap/RangedExec).
    thread_local bool t_draining_own_queue = false;

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
        std::atomic<uint64_t> protect_calls{0};
        std::atomic<uint64_t> protect_nanos{0};
        std::atomic<uint64_t> queue_ops{0};
        std::atomic<uint64_t> queue_nanos{0};
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
        icicle_vcpu(icicle_emulator* emu, icicle_x86_64_emulator& machine, const uint32_t index)
            : emu_(emu),
              machine_(machine),
              index_(index)
        {
        }

        size_t index() const override
        {
            return this->index_;
        }

        memory_interface& memory() override;
        const memory_interface& memory() const override;

        void start(size_t count) override;

        void stop() override
        {
            this->stop_requested_ = true;
            icicle_stop(this->emu_);
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
            return icicle_write_register(this->emu_, reg, value, size);
        }

        size_t read_raw_register(const int reg, void* value, const size_t size) override
        {
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
        std::exception_ptr pending_hook_exception_{};

        // Step 6.2 (mirrors WHP whp_vcpu::run_active_/stop_requested_): run_active_ is true only while this
        // vCPU is inside icicle run() (a peer's stop-the-world waits for every peer's run_active_ to clear
        // before touching its VM). stop_requested_ distinguishes a REAL stop (thread-switch/external) from a
        // quiesce cancel (a peer's cross-VM mutation), so start() knows whether to return or re-enter.
        std::atomic_bool run_active_{false};
        std::atomic_bool stop_requested_{false};

        // Activity telemetry (progress meter): retired-instruction count and last-parked RIP,
        // published by the OWNING worker thread at its between-quanta safe point (VM parked there,
        // so the reads that fill them are race-free). Readers poll vcpu_activity() without
        // stopping peers.
        std::atomic_uint64_t published_instructions_{0};
        std::atomic_uint64_t published_rip_{0};
        smp_profile_counters smp_profile_{};
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
            this->quiesce_cancel_.assign(vcpu_count, 0);
            this->quantum_kick_.assign(vcpu_count, 0);
            this->issuer_seq_.assign(vcpu_count, 0);
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

            // Every vCPU VM needs the MMIO region; the callback context (ptr) is shared and coherent.
            this->apply_to_all_vms([=](icicle_emulator* h) { icicle_map_mmio(h, address, size, read_wrapper, ptr, write_wrapper, ptr); });
        }

        void map_memory(const uint64_t address, const size_t size, memory_permission permissions) override
        {
            const auto perm = static_cast<uint8_t>(permissions);
            if (this->vcpus_.size() == 1)
            {
                // Normal path: keeps COW + snapshots for the single-vCPU case (byte-identical to before SMP).
                ice(icicle_map_memory(this->emu_, address, size, perm), "Failed to map memory");
                return;
            }
            auto* self = static_cast<icicle_vcpu*>(t_running_vcpu);
            const bool caller_is_vcpu = self && &self->machine_ == this;
            if (!caller_is_vcpu)
            {
                // External/setup mutator: pause peers, allocate on the master, share into the rest directly.
                this->pause_peers_and([&] {
                    ice(icicle_map_smp_shared_fresh(this->emu_, address, size, perm), "Failed to map SMP memory");
                    for (size_t i = 1; i < this->vcpus_.size(); ++i)
                    {
                        ice(icicle_share_smp_pages(this->vcpus_[i]->handle(), this->emu_, address, size), "Failed to share SMP memory");
                    }
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
            icicle_emulator* const source = self->handle();
            const fanout_guard fanout{this->ops_in_flight_};
            this->ops_in_flight_.fetch_add(1, std::memory_order_acquire);
            ice(icicle_map_smp_shared_fresh(source, address, size, perm), "Failed to map SMP memory");
            void* const raw = icicle_smp_capture(source, address, size);
            ice(raw != nullptr, "Failed to capture SMP pages");
            const std::shared_ptr<void> captured(raw, icicle_smp_release_capture);
            const uint64_t seq = this->issuer_next(self->index());
            for (auto& v : this->vcpus_)
            {
                if (v.get() != self)
                {
                    this->queue_op(v->index(), seq, [captured, address](icicle_emulator* h) {
                        ice(icicle_smp_map_captured(h, captured.get(), address), "Failed to map captured SMP pages");
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
            // Same host pointer in every VM: coherent because they all alias the one host buffer.
            this->apply_to_all_vms(
                [=](icicle_emulator* h) { ice(icicle_map_host_memory(h, address, host_pointer, size, perm), "Failed to map host memory"); });
        }

        void flush_host_memory_cache(const void* host_pointer, const size_t size) override
        {
            this->apply_to_all_vms([=](icicle_emulator* h) { icicle_flush_host_memory_cache(h, host_pointer, size); });
        }

        bool map_shared_memory(const uint64_t address, const uint64_t source, const size_t size,
                               const memory_permission permissions) override
        {
            const auto perm = static_cast<uint8_t>(permissions);
            auto* self = static_cast<icicle_vcpu*>(t_running_vcpu);
            const bool caller_is_vcpu = self && &self->machine_ == this;
            if (this->vcpus_.size() == 1 || !caller_is_vcpu)
            {
                bool ok = true;
                this->pause_peers_and([&] {
                    for (auto& vcpu : this->vcpus_)
                    {
                        ok = (icicle_map_shared_memory(vcpu->handle(), address, source, size, perm) != 0) && ok;
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
            const uint64_t seq = this->issuer_next(self->index());
            for (auto& v : this->vcpus_)
            {
                if (v.get() != self)
                {
                    this->queue_op(v->index(), seq,
                                   [address, source, size, perm](icicle_emulator* h) { icicle_map_shared_memory(h, address, source, size, perm); });
                }
            }
            this->kick_peers(self->index());
            return ok;
        }

        void unmap_memory(const uint64_t address, const size_t size) override
        {
            this->apply_to_all_vms([=](icicle_emulator* h) { ice(icicle_unmap_memory(h, address, size), "Failed to unmap memory"); });
        }

        // 6.4 — the icicle handle to read/write guest memory through. During a syscall/hook the acting
        // vCPU (this thread's) handle is used: safe same-thread and coherent (all VMs share the smp pages),
        // instead of always the master whose VM may be executing on another thread. External threads (and
        // N=1, where the acting vCPU IS vcpus_[0]==emu_) fall back to the master.
        icicle_emulator* acting_handle() const
        {
            auto* v = static_cast<icicle_vcpu*>(t_running_vcpu);
            if (v && &v->machine_ == this)
            {
                return v->handle();
            }
            return this->emu_;
        }

        bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            const auto ok = icicle_read_memory(this->acting_handle(), address, data, size);
            if (ok || this->vcpus_.size() == 1)
            {
                return ok;
            }
            // 6.6: a BETWEEN-quantum worker READ races queued maps exactly like the write path
            // (the peer's loader map not yet applied on this vCPU). Drain own queue (+ bounded
            // in-flight wait) and RETRY the read. Mirrors write_memory's drain+retry closeout.
            auto* worker = static_cast<icicle_vcpu*>(t_worker_vcpu);
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
                return icicle_write_memory(this->acting_handle(), address, data, size);
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

            // SMP 6.6: if the written range is translated on ANY VM (detected via the shared PageData
            // IN_CODE_CACHE perms — visible from any single view), every OTHER VM's translation of it
            // must be dropped too, or a peer keeps executing stale code after this write.
            auto* self = static_cast<icicle_vcpu*>(t_running_vcpu);
            const bool caller_is_vcpu = self && &self->machine_ == this;
            const bool cached = icicle_code_range_is_cached(this->acting_handle(), address, size);
            if (smp_trace_enabled() && (cached || !caller_is_vcpu))
            {
                std::fprintf(stderr, "[SMPTRC] write addr=%#llx size=%zu cached=%d caller=%s\n",
                             (unsigned long long)address, size, (int)cached, caller_is_vcpu ? "vcpu" : "external");
            }

            if (!caller_is_vcpu)
            {
                // External caller: do NOT pause. A write through the master's view IS the coherent
                // shared state for SMP-shared guest RAM, so no peer handle needs touching for the
                // write itself. Pausing here DEADLOCKED: an external loader write waits for a peer's
                // run_active_ to clear, but the peer can be parked in a hook that cannot progress
                // (trace: "pause still waiting (44s) for: vcpu0" after write 0x101cd39fb20/1232).
                // If the range was translated, queue invalidate ops for every VM instead (peers
                // drain at their next quantum; the kick bounds the latency).
                bool ok = icicle_write_memory(this->emu_, address, data, size);
                if (!ok)
                {
                    // 6.6: the MAIN-thread loader write can hit a map that is queued for the
                    // MASTER but issued by a peer (the master's own queue). Drain the master's
                    // queue (+ in-flight wait) and retry - the write-RETRIED-OK trace showed the
                    // worker branch doing exactly this; the external branch needed it too.
                    this->drain_own_queue_with_inflight_wait(*this->vcpus_[0]);
                    ok = icicle_write_memory(this->emu_, address, data, size);
                    if (!ok && smp_trace_enabled())
                    {
                        std::fprintf(stderr, "[SMPTRC] write-RETRY-FAILED(external) addr=%#llx size=%zu inflight=%llu\n",
                                     (unsigned long long)address, size,
                                     (unsigned long long)this->ops_in_flight_.load(std::memory_order_acquire));
                    }
                }
                if (cached)
                {
                    // External writer: its own issuer slot (index == vcpus_.size(), beyond any vCPU).
                    const uint64_t eseq = this->issuer_next(this->vcpus_.size());
                    for (auto& v : this->vcpus_)
                    {
                        this->queue_op(v->index(), eseq, [address, size](icicle_emulator* h) {
                            (void)icicle_invalidate_code_range(h, address, size);
                        });
                    }
                    this->kick_peers(std::numeric_limits<size_t>::max()); // external: no own vCPU to skip
                }
                return ok;
            }

            const auto own = self->index();
            bool ok = icicle_write_memory(self->handle(), address, data, size);
            if (!ok)
            {
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
                    this->queue_op(v->index(), iseq, [address, size](icicle_emulator* h) {
                        (void)icicle_invalidate_code_range(h, address, size);
                    });
                }
                this->kick_peers(own);
            }
            return ok;
        }

        void write_memory(const uint64_t address, const void* data, const size_t size) override
        {
            const auto res = try_write_memory(address, data, size);
            if (res)
            {
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
                return;
            }
                        // 6.6c'': a BETWEEN-quantum worker write (t_running_vcpu null, but this thread owns a
            // vCPU via t_worker_vcpu). ROOT-CAUSE CLOSEOUT: the recurring 1232-byte CONTEXT write
            // fails Unmapped while its map is queued/in-flight; deferring only the FAULT left the
            // page zeroed (the write never retried) and ntdll later read the zero CONTEXT and
            // synthesized the terminal AV (VIENTRY=0 proved no icicle violation delivers it). So:
            // drain own queue (+ bounded in-flight wait) and RETRY THE WRITE; only if it still
            // fails, defer the fault (real guest-visible fault).
            auto* worker = static_cast<icicle_vcpu*>(t_worker_vcpu);
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
                    }
                }
                if (icicle_write_memory(worker->handle(), address, data, size))
                {
                    if (smp_trace_enabled())
                    {
                        std::fprintf(stderr, "[SMPTRC] write-RETRIED-OK addr=%#llx size=%zu vcpu=%zu\n",
                                     (unsigned long long)address, size, worker->index());
                    }
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
                    return;
                }
            }
            if (smp_trace_enabled())
            {
                std::fprintf(stderr, "[SMPTRC] write-FAIL ice-throw addr=%#llx size=%zu tid=%u\n",
                             (unsigned long long)address, size, (unsigned)(GetCurrentThreadId()));
            }
            ice(false, "Failed to write memory");
        }

        void apply_memory_protection(const uint64_t address, const size_t size, memory_permission permissions) override
        {
            const auto perm = static_cast<uint8_t>(permissions);
            auto* self = static_cast<icicle_vcpu*>(t_running_vcpu);
            const bool caller_is_vcpu = self && &self->machine_ == this;
            if (this->vcpus_.size() == 1 || !caller_is_vcpu)
            {
                this->apply_to_all_vms([=](icicle_emulator* h) { ice(icicle_protect_memory(h, address, size, perm), "Failed to apply permissions"); });
                return;
            }
            // SMP 6.6c'': STALE-OP GUARD. A deferred protect queued now can land on a peer AFTER
            // the range was freed and RE-MAPPED (the probe's 'Failed to write memory': a stale
            // protect over the loader's fresh module). Capture the range's perm epoch at queue
            // time; the peer's apply skips if the epoch changed (a newer map/protect won).
            const auto own = self->index();
            auto* profile = this->profile_enabled_ ? &self->smp_profile_ : nullptr;
            const smp_profile_timer protect_timer{profile ? &profile->protect_nanos : nullptr};
            if (profile)
            {
                profile->protect_calls.fetch_add(1, std::memory_order_relaxed);
            }
            const uint64_t epoch = icicle_perm_epoch_of_range(self->handle(), address, size);
            ice(icicle_protect_memory(self->handle(), address, size, perm), "Failed to apply permissions");
            const uint64_t pseq = this->issuer_next(own);
            for (auto& v : this->vcpus_)
            {
                if (v->index() != own)
                {
                    this->queue_op(v->index(), pseq, [this, address, size, perm, epoch, own, pseq](icicle_emulator* h) {
                        // Same-issuer sequences (this issuer's earlier map) always apply in order;
                        // the perm_epoch stale-skip applies only CROSS-issuer (the recording vCPU's
                        // own next seq > pseq means this protect was superseded by its own later op).
                        const uint64_t now_seq = this->issuer_seq_.size() > own ? this->issuer_seq_[own] : pseq + 1;
                        const bool superseded_by_same_issuer = now_seq > pseq + 1;
                        if (!superseded_by_same_issuer && icicle_perm_epoch_of_range(h, address, size) == epoch)
                        {
                            ice(icicle_protect_memory(h, address, size, perm), "Failed to apply permissions");
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
                std::unique_lock lock(this->partition_mutex_);
                this->hooks_to_delete_.insert(hook);
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
            const auto issued = this->ops_issued_watermark_.load(std::memory_order_acquire);
            uint64_t pending = 0;
            {
                std::lock_guard<std::mutex> lock(this->pending_mutex_);
                for (const auto& q : this->pending_ops_)
                {
                    pending += q.size();
                }
            }
            return issued >= pending && (issued - pending) >= mark;
        }

        void sync_worker_context(const size_t vcpu_index) override
        {
            // The scheduler passes the authoritative vCPU index: a worker that has not run its
            // first quantum yet has no thread-local registration, and its idle loop must still
            // drain its own queue (GATEDIAG livelock at N>=4: q_i stuck forever, all parked).
            if (vcpu_index < this->vcpus_.size())
            {
                auto& worker = *this->vcpus_[vcpu_index];
                this->drain_pending_ops(worker);
                // Progress-meter safe point: the caller owns this vCPU and its VM is parked
                // here, so both reads are race-free.
                worker.published_instructions_.store(icicle_get_icount(worker.emu_), std::memory_order_relaxed);
                worker.published_rip_.store(worker.read_instruction_pointer(), std::memory_order_relaxed);
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
            if (!this->profile_enabled_)
            {
                return {};
            }
            std::vector<smp_profile_snapshot> out;
            out.reserve(this->vcpus_.size());
            for (const auto& vcpu : this->vcpus_)
            {
                const auto& p = vcpu->smp_profile_;
                out.push_back(smp_profile_snapshot{
                    .map_calls = p.map_calls.load(std::memory_order_relaxed),
                    .map_nanos = p.map_nanos.load(std::memory_order_relaxed),
                    .protect_calls = p.protect_calls.load(std::memory_order_relaxed),
                    .protect_nanos = p.protect_nanos.load(std::memory_order_relaxed),
                    .queue_ops = p.queue_ops.load(std::memory_order_relaxed),
                    .queue_nanos = p.queue_nanos.load(std::memory_order_relaxed),
                    .kick_calls = p.kick_calls.load(std::memory_order_relaxed),
                    .kick_targets = p.kick_targets.load(std::memory_order_relaxed),
                    .kick_nanos = p.kick_nanos.load(std::memory_order_relaxed),
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
        {            std::string out{};
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
            return out;
        }

      private:
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
            bool pending{false};  // reserved handle for a hook queued from inside a running hook
            bool deleted{false};  // delete_hook seen; queued installs must not apply
        };
        std::unordered_map<emulator_hook*, std::shared_ptr<hook_registration>> registrations_{};

        icicle_emulator* emu_{};
        std::vector<std::unique_ptr<icicle_vcpu>> vcpus_{};
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
        std::mutex quiesce_mutex_{};
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
            std::function<void(icicle_emulator*)> apply{};
        };
        std::vector<std::vector<pending_op>> pending_ops_{}; // [vm index] -> ops(handle)
        // Next sequence number per issuer vCPU index (monotonic).
        std::vector<uint64_t> issuer_seq_{};
        // SMP 6.7 RC#2: count of ops ever queued to peers. applied(mark) = issued - pending
        // (per-target FIFO), so a host gate can wait for its earlier queued memory ops.
        std::atomic<uint64_t> ops_issued_watermark_{0};

        // 6.6: nonzero while ANY vCPU is between applying a cross-VM mutation to its own VM and
        // queueing it for peers. A peer faulting UNMAPPED with an empty queue can briefly wait for
        // the in-flight op to land (guest synchronization assumes maps are instantly coherent).
        std::atomic<uint64_t> ops_in_flight_{0};

        // Queue `op` for peer vCPU `target`, stamped with the ISSUING vCPU's sequence. External
        // (setup) callers issue as vCPU 0 semantics with their own counter — use size_t max issuer?
        // Simpler: external callers pass issuer = their own index sentinel via issuer_next().
        uint64_t issuer_next(const size_t issuer)
        {
            if (issuer >= this->issuer_seq_.size())
            {
                this->issuer_seq_.resize(issuer + 1, 0);
            }
            return this->issuer_seq_[issuer]++;
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

        void queue_op(const size_t target, const uint64_t seq, std::function<void(icicle_emulator*)> op)
        {
            auto* self = static_cast<icicle_vcpu*>(t_running_vcpu);
            auto* profile = this->profile_enabled_ && self && &self->machine_ == this ? &self->smp_profile_ : nullptr;
            const smp_profile_timer queue_timer{profile ? &profile->queue_nanos : nullptr};
            // Push under the mutex FIRST, bump the issued watermark AFTER: smp_op_applied() reads
            // issued then sums the queues under the same mutex, so a watermark bumped before the
            // push could make an un-queued op count as applied (the gate opens early - the 6.7
            // transient first-read race). This order errs the safe way: pending may briefly
            // exceed issued, which only holds the visibility gate one drain longer.
            {
                std::lock_guard<std::mutex> lock(this->pending_mutex_);
                if (target >= this->pending_ops_.size())
                {
                    return;
                }
                this->pending_ops_[target].push_back(pending_op{seq, std::move(op)});
            }
            this->ops_issued_watermark_.fetch_add(1, std::memory_order_acq_rel);
            if (profile)
            {
                profile->queue_ops.fetch_add(1, std::memory_order_relaxed);
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
            std::lock_guard<std::mutex> plock(this->pause_mutex_);
            this->run_with_vcpus_paused(std::forward<Fn>(fn));
        }

        // 6.5 routing rule: a mutation requested BY a vCPU of this machine (from its own hook, the
        // deferred-action one-shot, or its quantum-end drain — contexts that may hold the BEL) applies to
        // its OWN VM now and queues the op for each peer, drained on the peer's thread at
        // begin_run_quantum. An external/setup caller pauses peers instead — it holds no BEL, so a peer
        // parked in a hook can always acquire the BEL, finish, exit run(), and become stoppable. Because
        // vCPU threads never pause, the A-B/B-A hazard between partition_mutex_ and the quiesce wait
        // cannot form.
        void route_to_all_vms(const std::function<void(size_t)>& per_vm_op)
        {
            if (this->vcpus_.size() == 1)
            {
                per_vm_op(0);
                return;
            }
            auto* self = static_cast<icicle_vcpu*>(t_running_vcpu);
            const bool caller_is_vcpu = self && &self->machine_ == this;
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
                    this->queue_op(i, rseq, [this, i, per_vm_op](icicle_emulator*) { per_vm_op(i); });
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

            return handle;
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
            }
        }

        // 6.6: drain this vCPU's own queue; if empty while a cross-VM fanout is in flight,
        // bounded-wait for the in-flight op to land first (drain+retry's shared core).
        void drain_own_queue_with_inflight_wait(icicle_vcpu& v)
        {
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

        bool complete_run_quantum(icicle_vcpu& v)
        {
            v.run_active_ = false;
            if (this->vcpus_.size() == 1)
            {
                // N=1: honor a self-kick (deferred execution-hook install) by resuming the quantum —
                // begin_run_quantum drains it outside icicle run().
                std::lock_guard<std::mutex> lock(this->quiesce_mutex_);
                if (this->quantum_kick_[v.index_])
                {
                    this->quantum_kick_[v.index_] = 0;
                    return true;
                }
                return false;
            }
            std::unique_lock lock(this->quiesce_mutex_);
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
            return false;
        }

        // 6.5: after a vCPU queues cross-VM ops for peers (async map/protect/unmap), kick each RUNNING
        // peer so its current quantum ends promptly (icicle checks the stop flag at block boundaries) and
        // begin_run_quantum drains the queue. Without the kick a spinning peer could run its whole quantum
        // — faulting on a mapping whose pointer it can already observe — before draining. Residual race
        // (peer's last few blocks before the kick lands) is the documented 6.6 TLB-coherency window.
        void kick_peers(const size_t own_index)
        {
            auto* self = static_cast<icicle_vcpu*>(t_running_vcpu);
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
            const auto resume = utils::finally([this] {
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
                if (smp_trace_enabled() && waited_s > 0 && waited_s % 2 == 0)
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
            // Every peer is now outside icicle run(): safe to touch their VMs from this thread.
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
        this->stop_requested_ = false;
        t_worker_vcpu = this;
        t_running_vcpu = this;
        const auto clear_current = utils::finally([] { t_running_vcpu = nullptr; });
        // start(count) must run at most `count` instructions in TOTAL. A kick-ended quantum resumes
        // here, and re-issuing the full count would extend the budget past the caller's contract
        // (caught by RangedExecHook...: the resumed vCPU ran off the end of its code). Track the
        // remainder via the VM's retired-instruction counter. count==0 means unlimited (icicle contract).
        const uint64_t base_icount = icicle_get_icount(this->emu_);
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
            this->machine_.begin_run_quantum(*this);
            icicle_start(this->emu_, remaining);
            if (this->machine_.complete_run_quantum(*this))
            {
                continue; // paused for a peer's cross-VM mutation — resume this quantum
            }
            break;
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
