#include "native_marker_capture.hpp"
#include "std_include.hpp"
#include <atomic>
#include <cstdlib>
#include <native_marker_resource_memory.hpp>
#include <native_marker_request_pool_memory.hpp>
#include <windows_emulator.hpp>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sogen
{
    namespace
    {
        uint64_t marker_now()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        detail::native_marker_identity marker_identity(windows_emulator& win, const uint64_t generation,
                                                       const detail::native_marker_identity previous)
        {
            auto result = previous;
#ifdef _WIN32
            if (!result.birth)
            {
                FILETIME birth{}, exit{}, kernel{}, user{};
                if (GetProcessTimes(GetCurrentProcess(), &birth, &exit, &kernel, &user))
                {
                    result.pid = GetCurrentProcessId();
                    result.birth = (uint64_t{birth.dwHighDateTime} << 32) | birth.dwLowDateTime;
                    result.generation = generation;
                }
            }
#endif
            result.module_base = 0;
            result.module_size = 0;
            const auto* module = win.mod_manager.find_by_name("destiny2.exe");
            if (module && module->machine == 0x8664)
            {
                result.module_base = module->image_base;
                result.module_size = module->size_of_image;
            }
            return result;
        }

        struct marker_dispatch_guard
        {
            windows_emulator& win;

            bool is_held_by_current_thread() const
            {
                return win.has_active_dispatch_context();
            }
        };

        void qualify_layout(windows_emulator& win, detail::native_marker_snapshot& sample)
        {
            constexpr std::array<uint64_t, 2> rvas{0xB440DF, 0xFD1A10};
            constexpr std::array<uint8_t, 8> expected{0x41, 0xC6, 0x07, 0x02, 0x83, 0x79, 0x0C, 0xFF};
            for (size_t index = 0; index < rvas.size(); ++index)
            {
                const auto rva = rvas[index];
                if (sample.budget.expired(marker_now()) || rva > sample.identity.module_size || 4 > sample.identity.module_size - rva)
                {
                    continue;
                }
                const auto address = sample.identity.module_base + rva;
                if (!detail::inspection_passive_span(win.memory, address, 4) || sample.budget.expired(marker_now()))
                {
                    continue;
                }
                ++sample.qualification_reads;
                sample.qualification_bytes += 4;
                std::array<uint8_t, 4> bytes{};
                if (win.memory.try_read_memory(address, bytes.data(), bytes.size()))
                {
                    std::copy(bytes.begin(), bytes.end(), sample.layout_signature.begin() + index * 4);
                    sample.qualification_available |= static_cast<uint8_t>(1U << index);
                }
            }
            sample.layout_qualified = sample.qualification_available == 3 && sample.layout_signature == expected;
        }

        bool marker_core_complete(const detail::native_marker_snapshot& sample)
        {
            const auto& resources = sample.resources;
            return sample.layout_qualified && resources.resource3.bytes && resources.resource6.bytes && resources.outstanding.bytes &&
                   resources.active_pointer.bytes && resources.registry_pointer.bytes &&
                   (sample.registers.context.available[0] & 0x1FFFFULL) == 0x1FFFFULL && sample.registers.context.stack_status == 0 &&
                   !sample.budget.time_exhausted && !sample.budget.clock_invalid;
        }
    } // namespace

    native_marker_observer::native_marker_observer()
        : native_marker_observer([] {
              const auto* value = std::getenv("SOGEN_DESTINY_MARKER_CAPTURE");
              return value ? std::string_view{value} : std::string_view{};
          }())
    {
    }

    native_marker_observer::native_marker_observer(const std::string_view opt_in)
        : enabled_(opt_in == "1")
    {
        static std::atomic<uint64_t> sequence{};
        generation_ = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    std::optional<detail::native_marker_snapshot> native_marker_observer::capture(windows_emulator& win,
                                                                                  const std::string_view message) noexcept
    {
        if (!enabled_ || !win.has_active_dispatch_context() || coverage_sent_)
        {
            return std::nullopt;
        }
        std::optional<detail::native_marker_snapshot> result;
        try
        {
            if (!arm_attempted_ && detail::native_marker_capture_state::classify(message) != detail::native_marker_event::investment_entry)
            {
                return std::nullopt;
            }
            if (state_.phase() == detail::native_marker_phase::retired)
            {
                return std::nullopt;
            }
            const auto began = marker_now();
            const auto identity = marker_identity(win, generation_, state_.identity());
            if (!arm_attempted_)
            {
                arm_attempted_ = true;
                if (began > UINT64_MAX - detail::native_marker_limits::maximum_lease_ns ||
                    !state_.arm("1", identity, began, began + detail::native_marker_limits::maximum_lease_ns))
                {
                    coverage_sent_ = true;
                    result.emplace();
                    result->identity = identity;
                    result->error = "marker identity or bounded arm unavailable";
                    return result;
                }
            }
            const auto reservation = state_.observe(identity, message, began);
            if (!reservation)
            {
                if (state_.phase() == detail::native_marker_phase::retired)
                {
                    coverage_sent_ = true;
                    result.emplace();
                    result->identity = state_.identity();
                    result->completion.reason = state_.stop_reason();
                    result->error = "marker observer retired before next selected marker";
                    result->counts = state_.counts();
                }
                return result;
            }
            auto& sample = result.emplace();
            sample.reservation = reservation;
            sample.identity = identity;
            sample.budget.started_ns = began;
            qualify_layout(win, sample);
            marker_dispatch_guard guard{win};
            if (sample.layout_qualified)
            {
                sample.resources = detail::sample_native_marker_resource_memory(win.memory, identity.module_base, identity.module_size,
                                                                                guard, sample.budget, marker_now);
            }
            auto& cpu = win.active_cpu();
            sample.registers = detail::capture_native_marker_context(
                win.memory, static_cast<uint32_t>(cpu.index()), win.current_thread().id,
                [&](const x86_register reg, void* value, const size_t size) {
                    return sample.budget.expired(marker_now()) ? size_t{0} : cpu.read_register(reg, value, size);
                },
                [&](const uint64_t address, void* value, const size_t size) {
                    return sample.budget.try_reserve(size, marker_now()) && win.memory.try_read_memory(address, value, size);
                },
                [&](uint64_t, const size_t size) { return sample.budget.can_read(size, marker_now()); });
            sample.core_ended_steady_ns = marker_now();
            sample.budget.expired(sample.core_ended_steady_ns);
            sample.core_complete = marker_core_complete(sample);
            std::shared_ptr<detail::native_marker_pool_sample> pool;
            if (sample.core_complete && reservation.event == detail::native_marker_event::cleanup_entry)
            {
                sample.budget.allow_cleanup_pool();
                pool = std::make_shared<detail::native_marker_pool_sample>(detail::sample_native_marker_request_pool_memory(
                    win.memory, identity.module_base, identity.module_size, sample.resources, guard, sample.budget, marker_now));
            }
            sample.ended_steady_ns = marker_now();
            sample.budget.expired(sample.ended_steady_ns);
            if (pool && (sample.budget.time_exhausted || sample.budget.clock_invalid))
            {
                detail::native_marker_pool_unqualify(*pool, detail::native_marker_pool_reason::time_or_clock_limit);
            }
            sample.complete = sample.core_complete && !sample.budget.time_exhausted && !sample.budget.clock_invalid;
            sample.completion = state_.finish(identity, reservation.sequence, sample.complete, sample.ended_steady_ns);
            if (pool && !sample.completion.accepted)
            {
                detail::native_marker_pool_unqualify(*pool, detail::native_marker_pool_reason::capture_rejected);
            }
            sample.complete = sample.complete && sample.completion.accepted && sample.completion.complete;
            sample.counts = state_.counts();
            sample.request_pool = std::move(pool);
            return result;
        }
        catch (...)
        {
            coverage_sent_ = true;
            if (result && result->reservation)
            {
                try
                {
                    result->ended_steady_ns = marker_now();
                    result->budget.expired(result->ended_steady_ns);
                    result->completion = state_.finish(result->identity, result->reservation.sequence, false, result->ended_steady_ns);
                }
                catch (...)
                {
                    result->budget.clock_invalid = true;
                }
            }
            state_.cancel();
            if (result)
            {
                try
                {
                    result->complete = false;
                    result->error = "marker cancel";
                    result->completion = {false, false, detail::native_marker_stop::cancelled};
                    result->counts = state_.counts();
                }
                catch (...)
                {
                    return std::nullopt;
                }
            }
            return result;
        }
    }
} // namespace sogen
