#define ICICLE_EMULATOR_IMPL
#include "icicle_x86_64_emulator.hpp"
#include "execution_hook.hpp"

#include <cstdio>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>
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

        using detail::hook_scope;

        template <typename T>
        struct function_object : utils::object
        {
            bool* hook_state{};
            std::function<T> func{};

            function_object(std::function<T> f = {}, bool* state = nullptr)
                : hook_state(state),
                  func(std::move(f))
            {
            }

            template <typename... Args>
            auto operator()(Args&&... args) const
            {
                const hook_scope scope(this->hook_state);

                return this->func.operator()(std::forward<Args>(args)...);
            }

            ~function_object() override = default;
        };

        template <typename T>
        std::unique_ptr<function_object<T>> make_function_object(std::function<T> func, bool& hook_state)
        {
            return std::make_unique<function_object<T>>(std::move(func), &hook_state);
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
        void throw_if_unhandled_stop();

        icicle_emulator* emu_{};
        icicle_x86_64_emulator& machine_;
        uint32_t index_{0};
        std::exception_ptr pending_hook_exception_{};
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
        }

        ~icicle_x86_64_emulator() override
        {
            // Free hook/storage objects first (nothing is running), then tear down every VM handle.
            this->registrations_.clear();
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
            for (auto& vcpu : this->vcpus_)
            {
                icicle_map_mmio(vcpu->handle(), address, size, read_wrapper, ptr, write_wrapper, ptr);
            }
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
            // SMP: allocate the shared page set on the master, alias the same Arc<PageData> into the rest
            // so all vCPUs share one coherent guest RAM (cachable, write-through, host-MESI coherency).
            ice(icicle_map_smp_shared_fresh(this->emu_, address, size, perm), "Failed to map SMP memory");
            for (size_t i = 1; i < this->vcpus_.size(); ++i)
            {
                ice(icicle_share_smp_pages(this->vcpus_[i]->handle(), this->emu_, address, size), "Failed to share SMP memory");
            }
        }

        void map_host_memory(const uint64_t address, const size_t size, void* host_pointer, memory_permission permissions) override
        {
            const auto perm = static_cast<uint8_t>(permissions);
            for (auto& vcpu : this->vcpus_)
            {
                // Same host pointer in every VM: coherent because they all alias the one host buffer.
                ice(icicle_map_host_memory(vcpu->handle(), address, host_pointer, size, perm), "Failed to map host memory");
            }
        }

        void flush_host_memory_cache(const void* host_pointer, const size_t size) override
        {
            for (auto& vcpu : this->vcpus_)
            {
                icicle_flush_host_memory_cache(vcpu->handle(), host_pointer, size);
            }
        }

        bool map_shared_memory(const uint64_t address, const uint64_t source, const size_t size,
                               const memory_permission permissions) override
        {
            const auto perm = static_cast<uint8_t>(permissions);
            bool ok = true;
            for (auto& vcpu : this->vcpus_)
            {
                ok = (icicle_map_shared_memory(vcpu->handle(), address, source, size, perm) != 0) && ok;
            }
            return ok;
        }

        void unmap_memory(const uint64_t address, const size_t size) override
        {
            for (auto& vcpu : this->vcpus_)
            {
                ice(icicle_unmap_memory(vcpu->handle(), address, size), "Failed to unmap memory");
            }
        }

        bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            return icicle_read_memory(this->emu_, address, data, size);
        }

        void read_memory(const uint64_t address, void* data, const size_t size) const override
        {
            const auto res = this->try_read_memory(address, data, size);
            ice(res, "Failed to read memory");
        }

        bool try_write_memory(const uint64_t address, const void* data, const size_t size) override
        {
            return icicle_write_memory(this->emu_, address, data, size);
        }

        void write_memory(const uint64_t address, const void* data, const size_t size) override
        {
            const auto res = try_write_memory(address, data, size);
            ice(res, "Failed to write memory");
        }

        void apply_memory_protection(const uint64_t address, const size_t size, memory_permission permissions) override
        {
            const auto perm = static_cast<uint8_t>(permissions);
            for (auto& vcpu : this->vcpus_)
            {
                ice(icicle_protect_memory(vcpu->handle(), address, size, perm), "Failed to apply permissions");
            }
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
                    return c(this->acting_cpu(vcpu_index), std::forward<Args>(args)...);
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
            auto& reg = this->registrations_[handle];
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto obj = make_function_object(this->bind_cpu(i, callback), this->is_in_hook_);
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
                reg.entries.emplace_back(i, id, std::move(obj));
            }

            return handle;
        }

        emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
        {
            auto* handle = this->fresh_hook_handle();
            auto& reg = this->registrations_[handle];
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto object = make_function_object(this->bind_cpu(i, callback), this->is_in_hook_);
                auto* ptr = object.get();
                auto* wrapper = +[](void* user, const uint64_t addr, const uint64_t instructions) {
                    basic_block block{};
                    block.address = addr;
                    block.instruction_count = static_cast<size_t>(instructions);

                    const auto& func = *static_cast<decltype(ptr)>(user);
                    (func)(block);
                };

                const auto id = icicle_add_block_hook(this->vcpus_[i]->handle(), wrapper, ptr);
                reg.entries.emplace_back(i, id, std::move(object));
            }

            return handle;
        }

        emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
        {
            auto* handle = this->fresh_hook_handle();
            auto& reg = this->registrations_[handle];
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto obj = make_function_object(this->bind_cpu(i, callback), this->is_in_hook_);
                auto* ptr = obj.get();
                auto* wrapper = +[](void* user, const int32_t code) {
                    const auto& func = *static_cast<decltype(ptr)>(user);
                    func(code);
                };

                const auto id = icicle_add_interrupt_hook(this->vcpus_[i]->handle(), wrapper, ptr);
                reg.entries.emplace_back(i, id, std::move(obj));
            }

            return handle;
        }

        emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override
        {
            auto* handle = this->fresh_hook_handle();
            auto& reg = this->registrations_[handle];
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto obj = make_function_object(this->bind_cpu(i, callback), this->is_in_hook_);
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
                reg.entries.emplace_back(i, id, std::move(obj));
            }

            return handle;
        }

        emulator_hook* hook_memory_execution(const uint64_t address, memory_execution_hook_callback callback) override
        {
            auto* handle = this->fresh_hook_handle();
            auto& reg = this->registrations_[handle];
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto object = std::make_unique<detail::execution_hook>(this->acting_cpu(i), callback, this->is_in_hook_,
                                                                       this->acting_sink(i));
                auto* ptr = object.get();
                auto* wrapper = +[](void* user, const uint64_t addr) {
                    const auto& func = *static_cast<decltype(ptr)>(user);
                    (func)(addr);
                };

                const auto id = icicle_add_execution_hook(this->vcpus_[i]->handle(), address, wrapper, ptr);
                reg.entries.emplace_back(i, id, std::move(object));
            }

            return handle;
        }

        emulator_hook* hook_memory_range_execution(const uint64_t address, const uint64_t size,
                                                   memory_execution_hook_callback callback) override
        {
            if (size == 1)
            {
                return this->hook_memory_execution(address, std::move(callback));
            }

            auto* handle = this->fresh_hook_handle();
            auto& reg = this->registrations_[handle];
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto object = std::make_unique<detail::execution_hook>(this->acting_cpu(i), callback, this->is_in_hook_,
                                                                       this->acting_sink(i));
                auto* ptr = object.get();
                auto* wrapper = +[](void* user, const uint64_t addr) {
                    const auto& func = *static_cast<decltype(ptr)>(user);
                    (func)(addr);
                };

                const auto id = icicle_add_ranged_execution_hook(this->vcpus_[i]->handle(), address, size, wrapper, ptr);
                reg.entries.emplace_back(i, id, std::move(object));
            }

            return handle;
        }

        emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override
        {
            auto* handle = this->fresh_hook_handle();
            auto& reg = this->registrations_[handle];
            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto object = std::make_unique<detail::execution_hook>(this->acting_cpu(i), callback, this->is_in_hook_,
                                                                       this->acting_sink(i));
                auto* ptr = object.get();
                auto* wrapper = +[](void* user, const uint64_t addr) {
                    const auto& func = *static_cast<decltype(ptr)>(user);
                    (func)(addr);
                };

                const auto id = icicle_add_generic_execution_hook(this->vcpus_[i]->handle(), wrapper, ptr);
                reg.entries.emplace_back(i, id, std::move(object));
            }

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
            std::unique_lock lock(this->partition_mutex_);
            if (this->is_in_hook_)
            {
                this->hooks_to_delete_.insert(hook);
            }
            else
            {
                this->delete_hook_internal(hook);
            }
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

      private:
        bool is_in_hook_{false};
        std::list<std::unique_ptr<utils::object>> storage_{};

        // One hook API handle maps to one icicle registration per vCPU VM: (vm index, icicle hook id, and
        // the object kept alive for that VM's captureless wrapper). delete_hook removes every VM's
        // registration; at N=1 this is exactly the old single (id, object) pair.
        struct hook_registration
        {
            std::vector<std::tuple<size_t, uint32_t, std::unique_ptr<utils::object>>> entries{};
            bool pending{false}; // reserved handle for a memory hook queued from inside a running hook
        };
        std::unordered_map<emulator_hook*, hook_registration> registrations_{};

        icicle_emulator* emu_{};
        std::vector<std::unique_ptr<icicle_vcpu>> vcpus_{};
        uint32_t index_{0};

        // icicle_vcpu::start() runs pending hook installs/deletes through the machine (hook machinery is
        // machine-scoped); each vCPU surfaces its own deferred hook exceptions.
        friend class icicle_vcpu;

        std::unordered_set<emulator_hook*> hooks_to_delete_{};
        std::unordered_map<emulator_hook*, memory_access_hook> hooks_to_install_{};

        // Step 6.1 (mirrors WHP whp_x86_64_emulator::partition_mutex_): guards the machine's shared hook
        // tables (registrations_/hooks_to_install_/hooks_to_delete_/index_) so the N vCPU worker threads
        // can mutate/consult them without racing. unique_lock to mutate, shared_lock to read (reads added
        // with the memory-routing work in 6.4). Lock order matches WHP: BEL -> partition_mutex, never the
        // reverse. Held only for C++ table work — never while invoking a guest/user callback.
        mutable std::shared_mutex partition_mutex_{};

        emulator_hook* fresh_hook_handle()
        {
            const auto id = ++this->index_;
            return reinterpret_cast<emulator_hook*>(static_cast<size_t>(id));
        }

        emulator_hook* hook_memory_access(memory_access_hook hook, emulator_hook* hook_id)
        {
            auto* handle = hook_id ? hook_id : this->fresh_hook_handle();
            auto& reg = this->registrations_[handle];
            reg.entries.clear();

            for (size_t i = 0; i < this->vcpus_.size(); ++i)
            {
                auto* const vm = this->vcpus_[i]->handle();
                std::unique_ptr<utils::object> object;
                uint32_t id{};
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
                        std::function<void(uint64_t, const void*, size_t, uint64_t, int32_t)>(std::move(shaped)), this->is_in_hook_);
                    auto* ptr = obj.get();
                    auto* wrapper = +[](void* user, uint64_t address, const void* data, size_t length, uint64_t error, int32_t host_write) {
                        (*static_cast<decltype(ptr)>(user))(address, data, length, error, host_write);
                    };
                    id = icicle_add_write_observation_hook(vm, hook.address, hook.address + hook.size, wrapper, ptr);
                    object = std::move(obj);
                }
                else
                {
                    auto obj = make_function_object(this->bind_cpu(i, hook.callback), this->is_in_hook_);
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
                reg.entries.emplace_back(i, id, std::move(object));
            }
            reg.pending = false;

            return handle;
        }

        void delete_hook_internal(emulator_hook* hook)
        {
            auto it = this->registrations_.find(hook);
            if (it == this->registrations_.end())
            {
                return;
            }

            if (it->second.pending)
            {
                // Reserved but not yet installed (queued from inside a running hook): cancel the install.
                this->hooks_to_install_.erase(hook);
                this->registrations_.erase(it);
                return;
            }

            for (auto& [vm_index, id, object] : it->second.entries)
            {
                icicle_remove_hook(this->vcpus_[vm_index]->handle(), id);
                (void)object;
            }
            this->registrations_.erase(it);
        }

        void perform_pending_actions()
        {
            // Runs at the end of every vCPU's start() on its worker thread → concurrent across vCPUs.
            std::unique_lock lock(this->partition_mutex_);
            const auto hooks_to_delete = std::move(this->hooks_to_delete_);

            this->hooks_to_delete_ = {};
            this->perform_pending_hook_installs();

            for (auto* hook : hooks_to_delete)
            {
                this->delete_hook_internal(hook);
            }
        }

        void perform_pending_hook_installs()
        {
            auto hooks_to_install = std::move(this->hooks_to_install_);
            this->hooks_to_install_ = {};

            for (auto& hook : hooks_to_install)
            {
                this->hook_memory_access(std::move(hook.second), hook.first);
            }
        }

        emulator_hook* try_install_memory_access_hook(memory_access_hook hook)
        {
            if (hook.size == 0 || hook.size > std::numeric_limits<uint64_t>::max() - hook.address)
            {
                throw std::invalid_argument("Invalid Icicle memory hook range");
            }
            std::unique_lock lock(this->partition_mutex_);
            if (!this->is_in_hook_)
            {
                return this->hook_memory_access(std::move(hook), nullptr);
            }

            auto* hook_id = this->fresh_hook_handle();
            this->registrations_[hook_id].pending = true;
            this->hooks_to_install_[hook_id] = std::move(hook);

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

            icicle_run_on_next_instruction(this->emu_, callback, heap_func);
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
        icicle_start(this->emu_, count);
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
