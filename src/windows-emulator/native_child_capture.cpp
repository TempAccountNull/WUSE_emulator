#include "std_include.hpp"
#include "native_child_capture.hpp"
#include "destiny_startup_capture.hpp"
#include "windows_emulator.hpp"
#include <cstdlib>
#include <cstring>
#include <utils/finally.hpp>

namespace sogen
{
    namespace
    {
        bool enabled_environment(const char* name)
        {
            const auto* value = std::getenv(name);
            return value && std::strcmp(value, "1") == 0;
        }
    }

    uint64_t native_child_capture::now()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    bool native_child_capture::enabled()
    {
        return enabled_environment("SOGEN_DESTINY_CHILD_CAPTURE");
    }

    std::shared_ptr<native_child_capture> native_child_capture::create(windows_emulator& owner, kernel_lock& kernel,
                                                                       const detail::native_marker_snapshot& marker)
    {
        return create(owner, kernel, marker, {});
    }

    std::shared_ptr<native_child_capture> native_child_capture::create(windows_emulator& owner, kernel_lock& kernel,
                                                                       const detail::native_marker_snapshot& marker,
                                                                       dependencies operations)
    {
        if (!enabled() || !kernel.is_held_by_current_thread())
        {
            return {};
        }
        const auto selected = detail::select_native_child(marker);
        if (!selected)
        {
            owner.log.warn("NATIVECHILD selection_unavailable\n");
            return {};
        }
        auto result = std::shared_ptr<native_child_capture>(new native_child_capture(owner, kernel, *selected, std::move(operations)));
        detail::native_child_sample sample{};
        sample.kind = detail::native_child_kind::selection;
        if (destiny_startup_capture::enabled())
        {
            sample.reason = detail::native_child_reason::profile_conflict;
            result->emit(std::move(sample), true);
            return {};
        }
        if (!result->emit(std::move(sample), true))
        {
            return {};
        }
        const std::weak_ptr<native_child_capture> weak = result;
        owner.callbacks.on_module_unload.add([weak](const mapped_module& module) {
            if (const auto self = weak.lock(); self && module.image_base == self->selected_.identity.module_base)
            {
                const auto current = now();
                for (uint32_t site = 0; site < 2; ++site)
                {
                    self->retire(site, detail::native_child_reason::module_unload, current);
                }
                self->service(self->owner_.active_cpu().index());
            }
        });
        const auto worker = [result_ptr = result.get()](const std::stop_token stop) { result_ptr->maintenance(stop); };
        result->worker_ = result->operations_.start_worker ? result->operations_.start_worker(worker) : std::jthread(worker);
        result->service(owner.active_cpu().index());
        return result;
    }

    native_child_capture::native_child_capture(windows_emulator& owner, kernel_lock& kernel, detail::native_child_selection selected,
                                               dependencies operations)
        : owner_(owner),
          kernel_(kernel),
          selected_(std::move(selected)),
          operations_(std::move(operations)),
          last_ns_(selected_.selected_ns)
    {
    }

    native_child_capture::~native_child_capture()
    {
        shutdown();
    }

    bool native_child_capture::same_module() const
    {
        const auto* module = owner_.mod_manager.find_by_name("destiny2.exe");
        return module && module->machine == 0x8664 && module->image_base == selected_.identity.module_base &&
               module->size_of_image == selected_.identity.module_size;
    }

    bool native_child_capture::quiescent(const size_t executing_cpu) const
    {
        for (size_t index = 0; index < owner_.vcpu_count(); ++index)
        {
            if (index != executing_cpu && owner_.vcpu(index).running.load(std::memory_order_relaxed))
            {
                return false;
            }
        }
        return true;
    }

    bool native_child_capture::emit(detail::native_child_sample sample, const bool lifecycle)
    {
        if (emission_closed_.load(std::memory_order_acquire))
        {
            return false;
        }
        auto& records = lifecycle ? lifecycle_records_ : detail_records_;
        auto& bytes = lifecycle ? lifecycle_bytes_ : detail_bytes_;
        const auto record_limit = lifecycle ? detail::native_child_limits::lifecycle_records : detail::native_child_limits::detail_records;
        const auto byte_limit = lifecycle ? detail::native_child_limits::lifecycle_bytes : detail::native_child_limits::detail_bytes;
        if (records >= record_limit || bytes >= byte_limit)
        {
            ++output_rejections_;
            return false;
        }
        sample.selection = selected_;
        sample.sequence = ++sequence_;
        sample.sites = counts_;
        for (size_t index = 0; index < sample.sites.size(); ++index)
        {
            sample.sites[index].raw_traps = raw_traps_[index].load(std::memory_order_relaxed);
        }
        sample.collection_ns = collection_ns_;
        sample.callback_ns = callback_ns_;
        sample.output_bytes = detail_bytes_ + lifecycle_bytes_;
        sample.output_rejections = output_rejections_;
        sample.removal_accepted = removal_accepted_;
        if (!sample.began_ns)
        {
            sample.began_ns = now();
            sample.ended_ns = sample.began_ns;
        }
        try
        {
            const auto payload = std::make_shared<const detail::native_child_sample>(std::move(sample));
            const auto charged = owner_.callbacks.on_native_child_capture(payload, byte_limit - bytes);
            if (!charged || charged > byte_limit - bytes)
            {
                ++output_rejections_;
                return false;
            }
            bytes += charged;
            ++records;
            return true;
        }
        catch (...)
        {
            ++output_rejections_;
            return false;
        }
    }

    void native_child_capture::retire(const uint32_t site, const detail::native_child_reason reason, const uint64_t current)
    {
        auto& state = counts_[site];
        if (state.retired_ns)
        {
            return;
        }
        state.retired_ns = current;
        state.retirement = reason;
        detail::native_child_sample sample{};
        sample.kind = detail::native_child_kind::coverage;
        sample.reason = reason;
        sample.site = site;
        emit(std::move(sample), true);
    }

    void native_child_capture::service(const size_t executing_cpu)
    {
        if (!kernel_.is_held_by_current_thread())
        {
            return;
        }
        const auto current = now();
        const auto invalid_clock = current < last_ns_ || current < selected_.selected_ns;
        last_ns_ = current;
        const auto identity_valid = same_module();
        for (uint32_t site = 0; site < 2; ++site)
        {
            const auto lease = site ? detail::native_child_limits::release_lease_ns : detail::native_child_limits::poll_lease_ns;
            if (shutting_down_ || invalid_clock || !identity_valid || current - selected_.selected_ns >= lease ||
                collection_ns_ >= detail::native_child_limits::collection_ns || callback_ns_ >= detail::native_child_limits::callback_ns)
            {
                const auto reason = shutting_down_                             ? detail::native_child_reason::shutdown
                                    : invalid_clock                            ? detail::native_child_reason::clock_invalid
                                    : !identity_valid                          ? detail::native_child_reason::identity_changed
                                    : current - selected_.selected_ns >= lease ? detail::native_child_reason::lease_expired
                                    : callback_ns_ >= detail::native_child_limits::callback_ns
                                        ? detail::native_child_reason::callback_cap
                                        : detail::native_child_reason::collection_cap;
                retire(site, reason, current);
            }
        }
        if (!activated_ && !counts_[1].retired_ns && quiescent(executing_cpu))
        {
            activated_ = true;
            detail::native_child_sample receipt{};
            receipt.kind = detail::native_child_kind::activation;
            receipt.began_ns = current;
            receipt.budget.started_ns = current;
            receipt.budget.read_limit = 3;
            receipt.budget.byte_limit = 130;
            const auto passive = [&](const uint64_t address, const size_t size) {
                return detail::inspection_passive_span(owner_.memory, address, size);
            };
            const auto read = [&](const uint64_t address, void* destination, const size_t size) {
                return owner_.memory.try_read_memory(address, destination, size);
            };
            const auto base = selected_.identity.module_base;
            const auto image_size = selected_.identity.module_size;
            if (image_size < 0xB440C4)
            {
                receipt.reason = detail::native_child_reason::identity_changed;
            }
            else
            {
                receipt.resolver_guard[0] = detail::native_marker_try_read<69>(base + 0x42C669, receipt.budget, passive, read, now);
                receipt.poll_guard = detail::native_marker_try_read<30>(base + 0x43C5D9, receipt.budget, passive, read, now);
                receipt.release_guard = detail::native_marker_try_read<31>(base + 0xB440A5, receipt.budget, passive, read, now);
                receipt.reason = !receipt.resolver_guard[0].bytes || !receipt.poll_guard.bytes || !receipt.release_guard.bytes
                                     ? detail::native_child_reason::guard_unavailable
                                 : *receipt.resolver_guard[0].bytes != detail::native_marker_pool_resolver_bytes ||
                                         *receipt.poll_guard.bytes != detail::native_child_poll_guard ||
                                         *receipt.release_guard.bytes != detail::native_child_release_guard
                                     ? detail::native_child_reason::guard_mismatch
                                     : detail::native_child_reason::available;
            }
            receipt.ended_ns = now();
            receipt.clock_accepted = !receipt.budget.expired(receipt.ended_ns);
            if (!receipt.clock_accepted)
            {
                receipt.reason = detail::native_child_reason::clock_invalid;
            }
            const auto reason = receipt.reason;
            const bool emitted = emit(std::move(receipt), true);
            if (reason != detail::native_child_reason::available || !emitted)
            {
                for (uint32_t site = 0; site < 2; ++site)
                {
                    retire(site, emitted ? reason : detail::native_child_reason::output_failure, now());
                }
            }
            else
            {
                const std::weak_ptr<native_child_capture> weak = shared_from_this();
                for (uint32_t site = 0; site < 2; ++site)
                {
                    if (counts_[site].retired_ns)
                    {
                        continue;
                    }
                    const auto install_now = now();
                    const auto lease = site ? detail::native_child_limits::release_lease_ns : detail::native_child_limits::poll_lease_ns;
                    if (install_now < current || install_now < selected_.selected_ns || !same_module() ||
                        install_now - selected_.selected_ns >= lease)
                    {
                        retire(site,
                               install_now < current || install_now < selected_.selected_ns ? detail::native_child_reason::clock_invalid
                               : !same_module()                                             ? detail::native_child_reason::identity_changed
                                                                                            : detail::native_child_reason::lease_expired,
                               install_now);
                        continue;
                    }
                    try
                    {
                        const auto mode = owner_.emu().get_name() == "Windows Hypervisor Platform"
                                              ? hook_interface::memory_execution_hook_mode::int3
                                              : hook_interface::memory_execution_hook_mode::automatic;
                        const auto callback = [weak, site](cpu_interface& cpu, const uint64_t rip) {
                            if (const auto self = weak.lock())
                            {
                                self->capture(cpu, rip, site);
                            }
                        };
                        const auto address = base + detail::native_child_limits::rvas[site];
                        hooks_[site] = operations_.install_hook ? operations_.install_hook(site, address, mode, callback)
                                                                : owner_.emu().hook_memory_execution_with_mode(address, mode, callback);
                        counts_[site].installed_ns = now();
                    }
                    catch (...)
                    {
                        retire(site, detail::native_child_reason::install_failure, now());
                    }
                }
            }
        }
        for (uint32_t site = 0; site < 2; ++site)
        {
            auto& state = counts_[site];
            if (!state.retired_ns || !hooks_[site])
            {
                continue;
            }
            if (current < state.retired_ns || current - state.retired_ns > detail::native_child_limits::removal_acceptance_ns)
            {
                removal_accepted_ = false;
            }
            ++state.removal_attempts;
            if (executing_cpu != SIZE_MAX || !quiescent(executing_cpu))
            {
                ++state.removal_deferrals;
                continue;
            }
            try
            {
                owner_.emu().delete_hook(hooks_[site]);
                hooks_[site] = nullptr;
                state.removed_ns = now();
                removal_accepted_ = removal_accepted_ && state.removed_ns >= state.retired_ns &&
                                    state.removed_ns - state.retired_ns <= detail::native_child_limits::removal_acceptance_ns;
                detail::native_child_sample sample{};
                sample.kind = detail::native_child_kind::removed;
                sample.reason = detail::native_child_reason::removed;
                sample.site = site;
                emit(std::move(sample), true);
            }
            catch (...)
            {
                removal_accepted_ = false;
            }
        }
        if (counts_[0].retired_ns && counts_[1].retired_ns && !hooks_[0] && !hooks_[1] && !final_sent_)
        {
            final_sent_ = true;
            detail::native_child_sample sample{};
            sample.kind = detail::native_child_kind::coverage;
            emit(std::move(sample), true);
        }
    }

    void native_child_capture::capture(cpu_interface& cpu, const uint64_t rip, const uint32_t site)
    {
        const auto began = now();
        raw_traps_[site].fetch_add(1, std::memory_order_relaxed);
        std::unique_lock guard(kernel_, std::defer_lock);
        if (!kernel_.is_held_by_current_thread())
        {
            guard.lock();
        }
        const auto account_callback = utils::finally([&] {
            const auto ended = now();
            callback_ns_ =
                detail::native_child_add_count(callback_ns_, ended >= began ? ended - began : detail::native_child_limits::callback_ns);
            try
            {
                service(cpu.index());
            }
            catch (...)
            {
                ++output_rejections_;
            }
        });
        auto& state = counts_[site];
        if (state.retired_ns || shutting_down_)
        {
            ++state.retired_traps;
            service(cpu.index());
            return;
        }
        ++state.active_traps;
        service(cpu.index());
        if (state.retired_ns)
        {
            return;
        }
        detail::native_marker_register_snapshot registers{};
        registers.actual_cpu = static_cast<uint32_t>(cpu.index());
        const auto& vcpu = owner_.vcpu(cpu.index());
        registers.actual_tid = vcpu.active_thread ? vcpu.active_thread->id : 0;
        detail::inspection_registers(
            [&](const x86_register name, void* value, const size_t size) {
                const auto permitted = name == x86_register::rax || name == x86_register::rbx || name == x86_register::rcx ||
                                       name == x86_register::rdx || name == x86_register::rsi || name == x86_register::rdi ||
                                       name == x86_register::rbp || name == x86_register::rsp || name == x86_register::r8 ||
                                       name == x86_register::r9 || name == x86_register::r10 || name == x86_register::r11 ||
                                       name == x86_register::r12 || name == x86_register::r13 || name == x86_register::r14 ||
                                       name == x86_register::r15 || name == x86_register::rip || name == x86_register::rflags;
                return permitted ? cpu.read_raw_register(static_cast<int>(name), value, size) : size_t{0};
            },
            registers.context);
        if (rip != registers.context.rip || !detail::native_child_prefilter(selected_, site, registers))
        {
            ++state.filtered;
        }
        else if (state.attempts < detail::native_child_limits::attempts_per_site)
        {
            ++state.attempts;
            detail::native_child_sample sample{};
            try
            {
                sample = detail::sample_native_child(
                    selected_, site, registers, kernel_,
                    [&](const uint64_t address, const size_t size) {
                        return detail::inspection_passive_span(owner_.memory, address, size);
                    },
                    [&](const uint64_t address, void* value, const size_t size) {
                        return owner_.memory.try_read_memory(address, value, size);
                    },
                    now, began);
            }
            catch (...)
            {
                sample.kind = site ? detail::native_child_kind::pre_release : detail::native_child_kind::post_poll;
                sample.site = site;
                sample.registers = registers;
                sample.began_ns = began;
                sample.ended_ns = now();
                sample.reason = detail::native_child_reason::read_unavailable;
                if (!site)
                {
                    sample.raw_return = static_cast<uint32_t>(registers.context.gpr[0]);
                    sample.manager = registers.context.gpr[15];
                    sample.context = registers.context.gpr[1];
                }
                else
                {
                    sample.effective_status = std::bit_cast<int32_t>(static_cast<uint32_t>(registers.context.gpr[1]));
                    sample.detail_return = static_cast<uint8_t>(registers.context.gpr[0]);
                    sample.effective_terminal = *sample.effective_status > 1;
                    sample.manager = registers.context.gpr[2];
                }
            }
            const auto ended = now();
            const auto elapsed = ended >= began ? ended - began : detail::native_child_limits::collection_ns;
            collection_ns_ = detail::native_child_add_count(collection_ns_, elapsed);
            const auto lease = site ? detail::native_child_limits::release_lease_ns : detail::native_child_limits::poll_lease_ns;
            const auto invalid_clock = ended < began || ended < selected_.selected_ns;
            const auto identity_valid = same_module();
            const auto lease_expired = !invalid_clock && ended - selected_.selected_ns >= lease;
            if (invalid_clock || !identity_valid || lease_expired || collection_ns_ >= detail::native_child_limits::collection_ns)
            {
                sample.child_qualified = false;
                sample.resource_identity = false;
                if (invalid_clock)
                {
                    sample.clock_accepted = false;
                }
                sample.context_word.reset();
                sample.reason = invalid_clock     ? detail::native_child_reason::clock_invalid
                                : !identity_valid ? detail::native_child_reason::identity_changed
                                : lease_expired   ? detail::native_child_reason::lease_expired
                                                  : detail::native_child_reason::collection_cap;
                if (sample.pool)
                {
                    try
                    {
                        auto pool = std::make_shared<detail::native_marker_pool_sample>(*sample.pool);
                        detail::native_marker_pool_unqualify(*pool, detail::native_marker_pool_reason::capture_rejected);
                        sample.pool = std::move(pool);
                    }
                    catch (...)
                    {
                        sample.pool.reset();
                    }
                }
                retire(site, sample.reason, ended);
            }
            const auto qualified = sample.child_qualified;
            const auto release_identity = sample.resource_identity && site == 1;
            if (!emit(std::move(sample), false))
            {
                for (uint32_t index = 0; index < 2; ++index)
                {
                    retire(index, detail::native_child_reason::output_failure, now());
                }
            }
            else if (qualified || release_identity)
            {
                ++state.accepted;
                retire(site, site ? detail::native_child_reason::first_selected_release : detail::native_child_reason::first_qualified_poll,
                       now());
                if (site)
                {
                    retire(0, detail::native_child_reason::first_selected_release, now());
                }
            }
        }
        if (state.active_traps >= detail::native_child_limits::trap_caps[site] ||
            state.attempts >= detail::native_child_limits::attempts_per_site)
        {
            retire(site,
                   state.active_traps >= detail::native_child_limits::trap_caps[site] ? detail::native_child_reason::trap_cap
                                                                                      : detail::native_child_reason::attempt_cap,
                   now());
        }
    }

    void native_child_capture::maintenance(const std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            std::unique_lock guard(kernel_, std::try_to_lock);
            if (guard.owns_lock())
            {
                service();
                if (final_sent_)
                {
                    return;
                }
            }
            if (guard.owns_lock())
            {
                guard.unlock();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    bool native_child_capture::shutdown() noexcept
    {
        worker_.request_stop();
        bool worker_stopped{};
        try
        {
            if (worker_.joinable())
            {
                if (worker_.get_id() == std::this_thread::get_id())
                {
                    return false;
                }
                worker_.join();
            }
            worker_stopped = true;
            std::unique_lock guard(kernel_, std::defer_lock);
            if (!kernel_.is_held_by_current_thread())
            {
                guard.lock();
            }
            shutting_down_ = true;
            service();
            if (!final_sent_)
            {
                final_sent_ = true;
                detail::native_child_sample sample{};
                sample.kind = detail::native_child_kind::coverage;
                sample.reason = detail::native_child_reason::shutdown;
                emit(std::move(sample), true);
            }
            emission_closed_.store(true, std::memory_order_release);
            return !hooks_[0] && !hooks_[1];
        }
        catch (...)
        {
            if (worker_stopped)
            {
                emission_closed_.store(true, std::memory_order_release);
            }
            return false;
        }
    }

    bool native_child_capture::reporting_stopped() const noexcept
    {
        return emission_closed_.load(std::memory_order_acquire);
    }

    bool native_child_capture::snapshot_safe() const noexcept
    {
        return !hooks_[0] && !hooks_[1];
    }

    native_child_capture_scope::native_child_capture_scope(windows_emulator& owner)
        : owner_(owner)
    {
    }

    native_child_capture_scope::~native_child_capture_scope()
    {
        finish();
    }

    bool native_child_capture_scope::finish() noexcept
    {
        if (!finished_)
        {
            complete_ = owner_.shutdown_native_child_capture();
            finished_ = true;
        }
        return complete_;
    }
}
