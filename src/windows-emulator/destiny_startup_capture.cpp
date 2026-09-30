#include "std_include.hpp"
#include "destiny_startup_capture.hpp"
#include "windows_emulator.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sogen
{
    namespace
    {
        enum observation_kind : uint8_t
        {
            baseline,
            activation,
            gate,
            signature,
            unavailable,
            retirement,
            coverage,
            removed,
            cleanup_timer
        };

        enum retirement_reason : uint8_t
        {
            active,
            total_budget,
            cleanup_budget,
            record_budget,
            byte_budget,
            module_unload,
            destruction,
            positive_gate,
            hit_budget,
            callback_budget,
            install_failure,
            output_failure,
            reason175_closure
        };

        enum observation_flag : uint64_t
        {
            read_failed = 1,
            identity_miss = 2,
            matched = 4,
            pre_release = 8,
            terminal_status_three = 16,
            earlier_unobserved = 32,
            native_generation_unavailable = 64,
            sender_matched = 128,
            native_transaction_assignment_matched = 256,
            native_transaction_assignment_unavailable = 512,
            investment_cleanup_transition_matched = 1024,
            reason175_ambiguous = 2048,
            reason175_producer_verified = 4096,
            reason175_setter_verified = 8192,
            reason175_commit_verified = 16384
        };

        struct capture_site
        {
            uint64_t rva;
            std::array<uint8_t, 8> bytes;
            size_t length;
            const char* name;
            const char* fields;
        };

        constexpr std::array sites{
            capture_site{.rva = 0xB440BF,
                         .bytes = {0xE8, 0x4C, 0x12, 0x8E, 0xFF},
                         .length = 5,
                         .name = "resource_pre_free",
                         .fields = "handle,slot,status,tag,state,detail,outstanding,wrapper,child,owner"},
            capture_site{.rva = 0xB440CC,
                         .bytes = {0x83, 0xFB, 0x03},
                         .length = 3,
                         .name = "resource_terminal",
                         .fields = "handle,slot,status,tag,state,detail,outstanding,wrapper,child,owner"},
            capture_site{.rva = 0xB440DF,
                         .bytes = {0x41, 0xC6, 0x07, 0x02},
                         .length = 4,
                         .name = "resource_state2_intent",
                         .fields = "handle,slot,status,tag,state,detail,outstanding,wrapper,child,owner"},
            capture_site{.rva = 0xB440E6,
                         .bytes = {0xE8, 0xD5, 0x0D, 0x00, 0x00},
                         .length = 5,
                         .name = "resource_install_call",
                         .fields = "handle,slot,status,tag,state,detail,outstanding,wrapper,child,owner"},
            capture_site{.rva = 0xB44F62,
                         .bytes = {0xE8, 0x99, 0xBE, 0x1C, 0x00},
                         .length = 5,
                         .name = "resource6_registry_install_call",
                         .fields = "tag,slot"},
            capture_site{.rva = 0x43C5E1,
                         .bytes = {0x41, 0x8B, 0x97, 0x68, 0x00, 0x22, 0x00},
                         .length = 7,
                         .name = "resource_native_post_poll",
                         .fields = "handle,child,transaction,poll,wrapper,child_object,context,owner,state,changed,processing,tag"},
            capture_site{.rva = 0x50BB0E,
                         .bytes = {0x41, 0x89, 0x50, 0x0C},
                         .length = 4,
                         .name = "registry_content_store_intent",
                         .fields = "object,content_argument,mode,current_content,vtable"},
            capture_site{.rva = 0x50BB19,
                         .bytes = {0x48, 0x89, 0x05, 0x80, 0x74, 0x23, 0x02},
                         .length = 7,
                         .name = "registry_active_publish_intent",
                         .fields = "object,content,mode,vtable"},
            capture_site{.rva = 0xFD1A10,
                         .bytes = {0x83, 0x79, 0x0C, 0xFF},
                         .length = 4,
                         .name = "registry_content_compare",
                         .fields = "object,content,mode,vtable"},
            capture_site{.rva = 0xFA15E8,
                         .bytes = {0x88, 0x48, 0x01},
                         .length = 3,
                         .name = "event26_flag_write_intent",
                         .fields = "native_view,payload,previous_flag"},
            capture_site{.rva = 0xB60220,
                         .bytes = {0xE8, 0xCB, 0x0C, 0x1B, 0x00},
                         .length = 5,
                         .name = "engine_active_tick_call",
                         .fields = "native_visit"},
            capture_site{.rva = 0xD10EF6,
                         .bytes = {0xE8, 0x25, 0x00, 0xED, 0xFF},
                         .length = 5,
                         .name = "module_tick_call",
                         .fields = "tick_argument"},
            capture_site{.rva = 0xBE103B,
                         .bytes = {0x38, 0x18},
                         .length = 2,
                         .name = "module_tick_flag_compare",
                         .fields = "native_view,manager,flag,comparison_zero"},
            capture_site{.rva = 0xBE109B,
                         .bytes = {0xFF, 0x50, 0x40},
                         .length = 3,
                         .name = "queuez_module_predicate_call",
                         .fields = "holder,vtable,context4,context6"},
            capture_site{.rva = 0xBE10AF,
                         .bytes = {0xE8, 0x4C, 0x08, 0x00, 0x00},
                         .length = 5,
                         .name = "queuez_phase_call",
                         .fields = "category,tick_id,holder"},
            capture_site{
                .rva = 0xBE10BE, .bytes = {0xFF, 0x50, 0x30}, .length = 3, .name = "queuez_module_update_call", .fields = "holder,vtable"},
            capture_site{.rva = 0xBE6D40,
                         .bytes = {0xE9, 0xC2, 0xCE, 0x81, 0x06},
                         .length = 5,
                         .name = "queuez_protected_entry_visit",
                         .fields = "holder"},
            capture_site{.rva = 0xBE6D4D,
                         .bytes = {0xE8, 0xBE, 0x71, 0x3E, 0x00},
                         .length = 5,
                         .name = "queuez_body_accessor_call",
                         .fields = "holder"},
            capture_site{.rva = 0xBE6D52,
                         .bytes = {0x48, 0x8B, 0xC8},
                         .length = 3,
                         .name = "queuez_native_manager_return",
                         .fields = "manager,state,identity"},
            capture_site{.rva = 0xBE6D60,
                         .bytes = {0xE8, 0xBB, 0x28, 0x00, 0x00},
                         .length = 5,
                         .name = "queuez_primary_maintenance_call",
                         .fields = "manager,state,identity"},
            capture_site{.rva = 0xBE9646,
                         .bytes = {0x80, 0x78, 0x38, 0x00},
                         .length = 4,
                         .name = "proxy_maintenance_valid_compare",
                         .fields = "native_proxy,identity,valid,primary_manager"},
            capture_site{.rva = 0xBE6D68,
                         .bytes = {0xE8, 0xE3, 0x27, 0x00, 0x00},
                         .length = 5,
                         .name = "queuez_primary_pump_call",
                         .fields = "manager,state,identity"},
            capture_site{.rva = 0xBE95C2,
                         .bytes = {0xE8, 0x39, 0xE8, 0xFF, 0xFF},
                         .length = 5,
                         .name = "primary_state1_sender_call",
                         .fields = "manager,record,state,identity,caller_rsp"},
            capture_site{.rva = 0xBE7E23,
                         .bytes = {0x84, 0xC0},
                         .length = 2,
                         .name = "sender_content_predicate_return",
                         .fields = "record,native_result"},
            capture_site{.rva = 0xBE7E36,
                         .bytes = {0x41, 0xFF, 0x50, 0x10},
                         .length = 4,
                         .name = "sender_descriptor_accessor_call",
                         .fields = "object,vtable"},
            capture_site{.rva = 0xBE7E3A,
                         .bytes = {0x48, 0x85, 0xC0},
                         .length = 3,
                         .name = "sender_descriptor_return",
                         .fields = "record,descriptor"},
            capture_site{.rva = 0xBE7E8B,
                         .bytes = {0xE8, 0x30, 0x3E, 0x34, 0x00},
                         .length = 5,
                         .name = "sender_payload_call",
                         .fields = "payload,record,identity"},
            capture_site{.rva = 0xF2BD07,
                         .bytes = {0xE8, 0x74, 0x05, 0x00, 0x00},
                         .length = 5,
                         .name = "literal503_dispatch_call",
                         .fields = "opcode,mode,descriptor,completion_token,arg5,caller_rsp,record,identity"},
            capture_site{.rva = 0xF2C2F5,
                         .bytes = {0xE8, 0xE6, 0x93, 0xF4, 0xFF},
                         .length = 5,
                         .name = "carrier_native_getter_call",
                         .fields = "selector,frame_matches"},
            capture_site{.rva = 0xF2C36A,
                         .bytes = {0x41, 0x83, 0xFC, 0x03},
                         .length = 4,
                         .name = "carrier_native_state_compare",
                         .fields = "peer_state,opcode,mode,descriptor,completion_token,frame_matches,record,identity"},
            capture_site{.rva = 0x1071F99,
                         .bytes = {0xE8, 0x12, 0xBF, 0xDB, 0xFF},
                         .length = 5,
                         .name = "task0_native_deadline_call",
                         .fields = "native_visit"},
            capture_site{.rva = 0x107289E,
                         .bytes = {0x83, 0xF8, 0x03},
                         .length = 3,
                         .name = "task0_native_result_compare",
                         .fields = "native_result"},
            capture_site{.rva = 0x10728D3,
                         .bytes = {0x83, 0xF8, 0x03},
                         .length = 3,
                         .name = "task2_native_result_compare",
                         .fields = "native_result"},
            capture_site{.rva = 0xB440D1,
                         .bytes = {0x41, 0xC6, 0x07, 0x04},
                         .length = 4,
                         .name = "resource_state4_intent",
                         .fields = "handle,slot,status,tag,state,detail,outstanding,wrapper,child,owner"},
            capture_site{.rva = 0x509B23,
                         .bytes = {0xC7, 0x43, 0x0C, 0xFF, 0xFF, 0xFF, 0xFF},
                         .length = 7,
                         .name = "registry_content_reset_intent",
                         .fields = "object,old_content,mode,vtable"},
            capture_site{.rva = 0x509B2D,
                         .bytes = {0x48, 0x89, 0x05, 0x6C, 0x94, 0x23, 0x02},
                         .length = 7,
                         .name = "registry_active_reset_intent",
                         .fields = "object,content,mode,active_before,zero_argument,vtable,current_registry_global"},
            capture_site{.rva = 0x4397F7,
                         .bytes = {0x66, 0x41, 0x89, 0x06},
                         .length = 4,
                         .name = "native_transaction_assignment_intent",
                         .fields = "handle,child,transaction,context,owner"},
            capture_site{.rva = 0x439805,
                         .bytes = {0x89, 0x9D, 0x68, 0x00, 0x22, 0x00},
                         .length = 6,
                         .name = "native_active_child_assignment_intent",
                         .fields = "handle,child,transaction,context,owner"},
            capture_site{.rva = 0xE1B0CF,
                         .bytes = {0x0F, 0x84, 0x11, 0x01, 0x00, 0x00},
                         .length = 6,
                         .name = "world_controller_cleanup_commit_before_enter",
                         .fields = "manager,previous_state,current_state,current_reason,native_timestamp,goal_state,goal_reason,native_rax,"
                                   "return_address,native_eflags,guards_verified,timer_previously_seen"},
        };
        constexpr std::array<uint8_t, 27> dispatcher_prologue{0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41,
                                                              0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0xD0, 0xFC,
                                                              0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x30, 0x04, 0x00, 0x00};
        constexpr std::array<const char*, 9> kind_names{"baseline",   "activation", "gate",    "signature",    "unavailable",
                                                        "retirement", "coverage",   "removed", "cleanup_timer"};
        constexpr std::array<const char*, 13> reason_names{"active",
                                                           "total_budget",
                                                           "post_cleanup_budget",
                                                           "record_budget",
                                                           "byte_budget",
                                                           "module_unload",
                                                           "destruction",
                                                           "positive_gate",
                                                           "hit_budget",
                                                           "callback_time_budget",
                                                           "install_failure",
                                                           "output_failure",
                                                           "first_request_closure_budget"};
        constexpr uint64_t resource_rva = 0x1FB5F20;
        constexpr uint64_t user_end = 0x800000000000;

        template <typename T, size_t N>
        T field(const std::array<uint8_t, N>& bytes, const size_t offset)
        {
            T result{};
            std::memcpy(&result, bytes.data() + offset, sizeof(result));
            return result;
        }

        bool user_range(const uint64_t address, const size_t length)
        {
            return address >= 0x10000 && address < user_end && length && length <= 4096 && length <= user_end - address;
        }
    }

    void destiny_startup_capture::arm_reason175_locked(const uint64_t now)
    {
        record activation_record{};
        activation_record.kind = activation;
        activation_record.recorder = 4;
        activation_record.host_ms = now;
        activation_record.values = {
            base_,          image_size_,  destiny_reason175_capture_state::total_ms,  destiny_reason175_capture_state::closure_ms,
            record_limit(), byte_limit(), destiny_reason175_capture_state::hit_limit, destiny_reason175_capture_state::callback_limit_ns};
        enqueue_locked(activation_record);
        bool verified = true;
        for (size_t index = 0; index < destiny_reason175_capture_state::dependencies.size(); ++index)
        {
            const auto& dependency = destiny_reason175_capture_state::dependencies[index];
            std::array<uint8_t, 75> bytes{};
            record check{};
            check.kind = signature;
            check.recorder = 4;
            check.site = 250;
            check.host_ms = now;
            check.rip = base_ + dependency.rva;
            const auto readable = dependency.rva <= image_size_ && dependency.length <= image_size_ - dependency.rva &&
                                  read(check.rip, bytes.data(), dependency.length, check);
            const auto match = readable && destiny_reason175_capture_state::dependency_matches(index, bytes.data(), dependency.length);
            check.values = {dependency.rva, dependency.length, index, readable, match};
            check.length = readable ? static_cast<uint8_t>(std::min(dependency.length, check.bytes.size())) : 0;
            std::copy_n(bytes.data(), check.length, check.bytes.data());
            check.flags |= match ? matched : identity_miss;
            verified = verified && match;
            enqueue_locked(check);
        }
        cleanup_guards_verified_ = verified;
        for (size_t index = 0; index < selected_site_count(); ++index)
        {
            auto& state = sites_[index];
            state.attempted = true;
            record check{};
            check.kind = signature;
            check.recorder = 4;
            check.site = static_cast<uint8_t>(index);
            check.host_ms = now;
            const auto rva = destiny_reason175_capture_state::rvas[index];
            const auto length = destiny_reason175_capture_state::lengths[index];
            check.rip = base_ + rva;
            check.length = static_cast<uint8_t>(length);
            const auto readable = rva <= image_size_ && length <= image_size_ - rva && read(check.rip, check.bytes.data(), length, check);
            check.values = {rva, length, index, readable,
                            readable && destiny_reason175_capture_state::guard_matches(index, check.bytes.data(), length)};
            check.length = readable ? static_cast<uint8_t>(length) : 0;
            if (!readable || !destiny_reason175_capture_state::guard_matches(index, check.bytes.data(), length))
            {
                ++state.signature_misses;
                state.read_failures += readable ? 0 : 1;
                check.flags |= identity_miss;
                verified = false;
            }
            else
            {
                check.flags |= matched;
            }
            enqueue_locked(check);
        }
        if (!verified)
        {
            retire_locked(install_failure, now);
            remove_hooks_locked();
            return;
        }
        const std::weak_ptr<destiny_startup_capture> weak = shared_from_this();
        try
        {
            for (size_t index = 0; index < selected_site_count(); ++index)
            {
                sites_[index].hook = owner_.emu().hook_memory_execution_with_mode(base_ + destiny_reason175_capture_state::rvas[index],
                                                                                  hook_interface::memory_execution_hook_mode::int3,
                                                                                  [weak, index](cpu_interface& cpu, const uint64_t rip) {
                                                                                      if (const auto self = weak.lock())
                                                                                      {
                                                                                          self->capture(cpu, rip, index);
                                                                                      }
                                                                                  });
                if (!sites_[index].hook)
                {
                    retire_locked(install_failure, now);
                    remove_hooks_locked();
                    return;
                }
            }
            record receipt{};
            receipt.kind = unavailable;
            receipt.recorder = 4;
            receipt.site = 249;
            receipt.host_ms = now_ms();
            receipt.values = {selected_site_count(), destiny_reason175_capture_state::dependencies.size(), cleanup_guards_verified_, now};
            enqueue_locked(receipt);
        }
        catch (...)
        {
            retire_locked(install_failure, now);
            remove_hooks_locked();
        }
    }

    void destiny_startup_capture::reason175_summary_locked(const uint64_t now)
    {
        if (reason175_summary_seen_)
        {
            return;
        }
        reason175_summary_seen_ = true;
        record summary{};
        summary.kind = unavailable;
        summary.recorder = 4;
        summary.site = 251;
        summary.host_ms = now;
        summary.token = reason175_.first_sequence;
        summary.tid = reason175_.tid;
        summary.values = {reason175_.reason,      reason175_.source,      reason175_.request_rsp,    reason175_.request_caller,
                          reason175_.manager,     reason175_.tid,         reason175_.first_sequence, reason175_.first_seen,
                          reason175_.setter_seen, reason175_.commit_seen, reason175_.ambiguous,      reason175_.producer_verified};
        if (reason175_.ambiguous)
        {
            summary.flags |= reason175_ambiguous;
        }
        if (reason175_.producer_verified)
        {
            summary.flags |= reason175_producer_verified;
        }
        if (reason175_.setter_seen)
        {
            summary.flags |= reason175_setter_verified;
        }
        if (reason175_.commit_seen)
        {
            summary.flags |= reason175_commit_verified;
        }
        if (reason175_.producer_verified && reason175_.setter_seen && reason175_.commit_seen && !reason175_.ambiguous)
        {
            summary.flags |= matched;
        }
        else
        {
            summary.flags |= earlier_unobserved;
        }
        enqueue_locked(summary, true);
    }

    void destiny_startup_capture::capture_reason175(cpu_interface& cpu, const uint64_t rip, const size_t site_index)
    {
        const auto begin = std::chrono::steady_clock::now();
        sites_[site_index].raw_traps.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock kernel_guard(kernel_, std::defer_lock);
        if (!kernel_.is_held_by_current_thread())
        {
            kernel_guard.lock();
        }
        const std::scoped_lock state_guard(mutex_);
        auto& state = sites_[site_index];
        if (retired_ || shutdown_ || state.retirement)
        {
            ++retired_traps_;
            remove_hooks_locked(cpu.index());
            return;
        }
        if (!state.hook)
        {
            return;
        }
        ++state.hits;
        const auto now = now_ms();
        const auto expired = reason175_.expiry(now);
        if (!expired.empty())
        {
            retire_locked(expired == "total_budget" ? total_budget : reason175_closure, now);
            remove_hooks_locked(cpu.index());
            return;
        }
        record observation{};
        observation.kind = gate;
        observation.recorder = 4;
        observation.site = static_cast<uint8_t>(site_index);
        observation.host_ms = now;
        observation.rip = rip;
        observation.vcpu = static_cast<uint32_t>(cpu.index());
        observation.token = ++serial_;
        bool emit = true;
        bool terminal{};
        bool emit_detail{};
        record detail{};
        try
        {
            auto& vcpu = owner_.vcpu(cpu.index());
            auto& acting = vcpu.cpu;
            observation.tid = vcpu.active_thread ? vcpu.active_thread->id : 0;
            const auto reg = [&](const x86_register name) { return acting.reg<uint64_t>(name); };
            const auto rsp = reg(x86_register::rsp);
            uint64_t mask{};
            const auto sampled = [&](const uint64_t address, const uint64_t offset, void* destination, const size_t length,
                                     const uint64_t bit) {
                const auto success = read_offset(address, offset, destination, length, observation);
                if (success)
                {
                    mask |= bit;
                }
                return success;
            };
            switch (site_index)
            {
            case 0: {
                const auto out_reason = reg(x86_register::rbx);
                const auto native_manager = reg(x86_register::rcx);
                const auto result = reg(x86_register::rax) & 0xFF;
                const auto rbp = reg(x86_register::rbp);
                uint32_t reason{};
                uint8_t latch{};
                uint64_t inner_return{};
                uint64_t middle_return{};
                uint64_t original_owner{};
                uint64_t outer_return{};
                sampled(out_reason, 0, &reason, sizeof(reason), 1);
                sampled(native_manager, 0x728, &latch, sizeof(latch), 2);
                sampled(rsp, 0x28, &inner_return, sizeof(inner_return), 4);
                sampled(rsp, 0x58, &middle_return, sizeof(middle_return), 8);
                sampled(rsp, 0x78, &original_owner, sizeof(original_owner), 16);
                sampled(rsp, 0x4C8, &outer_return, sizeof(outer_return), 32);
                observation.values = {out_reason,     reason,       native_manager, latch,         result, rsp,
                                      original_owner, outer_return, inner_return,   middle_return, rbp,    mask};
                const auto valid = destiny_reason175_capture_state::read_complete(mask, 63) && reason == 175 && result == 0 && rbp == 20 &&
                                   rsp <= user_end - 0x90 && out_reason == rsp + 0x90 && inner_return == base_ + 0xD48934 &&
                                   middle_return == base_ + 0xD4D8F5;
                if (blocked_reason_.seen)
                {
                    reason175_.ambiguous = true;
                }
                else
                {
                    blocked_reason_.seen = true;
                    blocked_reason_.valid = valid;
                    blocked_reason_.reason_pointer = out_reason;
                    blocked_reason_.owner = original_owner;
                    blocked_reason_.outer_return = outer_return;
                    blocked_reason_.tid = observation.tid;
                }
                observation.flags |= valid ? matched : identity_miss;
                break;
            }
            case 1: {
                const auto target = static_cast<uint32_t>(reg(x86_register::rcx));
                const auto reason = static_cast<uint32_t>(reg(x86_register::rdx));
                if (target != 28)
                {
                    emit = false;
                    break;
                }
                uint64_t caller{};
                sampled(rsp, 0, &caller, sizeof(caller), 1);
                const auto context = reg(x86_register::rsi);
                const auto config = reg(x86_register::rax);
                const auto owner = reg(x86_register::rdi);
                const auto rbp = reg(x86_register::rbp);
                uint32_t status{};
                int64_t elapsed{};
                int32_t limit{};
                bool producer{};
                uint8_t source{};
                if (caller == base_ + 0x1071F9E && reason == 175)
                {
                    source = 1;
                    sampled(context, 0, &status, sizeof(status), 2);
                    sampled(context, 8, &elapsed, sizeof(elapsed), 4);
                    sampled(config, 0x20, &limit, sizeof(limit), 8);
                    producer = destiny_reason175_capture_state::read_complete(mask, 15);
                }
                else if (caller == base_ + 0xD4DB8B && reason == 175)
                {
                    source = 2;
                    uint32_t consumed{};
                    uint64_t outer_return{};
                    sampled(rsp, 0x38, &consumed, sizeof(consumed), 16);
                    sampled(rsp, 0x470, &outer_return, sizeof(outer_return), 32);
                    uint64_t active_stage_mask{};
                    sampled(owner, 8, &active_stage_mask, sizeof(active_stage_mask), 64);
                    detail = observation;
                    detail.recorder = 5;
                    detail.values = {consumed,
                                     rsp + 0x38,
                                     outer_return,
                                     owner,
                                     active_stage_mask,
                                     mask,
                                     rsp,
                                     rbp,
                                     blocked_reason_.reason_pointer,
                                     blocked_reason_.owner,
                                     blocked_reason_.outer_return,
                                     blocked_reason_.tid};
                    emit_detail = true;
                    producer = destiny_reason175_capture_state::read_complete(mask, 49) && blocked_reason_.valid &&
                               blocked_reason_.tid == observation.tid && rsp <= user_end - 0x108 && rbp == rsp + 0x108 &&
                               blocked_reason_.reason_pointer == rsp + 0x38 && blocked_reason_.owner == owner &&
                               blocked_reason_.outer_return == outer_return && consumed == 175;
                }
                else
                {
                    source = 3;
                    producer = false;
                }
                observation.values = {
                    target, reason, rsp, caller, context, config, status, static_cast<uint64_t>(elapsed), static_cast<uint32_t>(limit),
                    mask,   owner,  rbp};
                if (reason175_.observe_request(observation.tid, reason, rsp, caller, now, observation.token))
                {
                    reason175_.source = source;
                    reason175_.producer_verified = producer;
                }
                else
                {
                    observation.flags |= reason175_ambiguous;
                }
                observation.flags |= producer ? reason175_producer_verified : earlier_unobserved;
                break;
            }
            case 2: {
                const auto target = static_cast<uint32_t>(reg(x86_register::rdx));
                if (target != 28)
                {
                    emit = false;
                    break;
                }
                const auto manager = reg(x86_register::rcx);
                const auto reason = static_cast<uint32_t>(reg(x86_register::r8));
                uint64_t caller{};
                uint64_t wrapper_caller{};
                sampled(rsp, 0, &caller, sizeof(caller), 1);
                if (caller == base_ + 0xE2E088)
                {
                    sampled(rsp, 0x450, &wrapper_caller, sizeof(wrapper_caller), 2);
                }
                const auto state_read = sampled(manager, 0x390, observation.bytes.data(), 24, 4);
                observation.length = state_read ? 24 : 0;
                observation.values = {manager,
                                      field<uint32_t>(observation.bytes, 0),
                                      field<uint32_t>(observation.bytes, 4),
                                      field<uint64_t>(observation.bytes, 8),
                                      field<uint32_t>(observation.bytes, 16),
                                      field<uint32_t>(observation.bytes, 20),
                                      target,
                                      reason,
                                      rsp,
                                      caller,
                                      wrapper_caller,
                                      mask};
                const auto frame_valid =
                    destiny_reason175_capture_state::read_complete(mask, 7) && caller == base_ + 0xE2E088 && rsp <= user_end - 0x450;
                if (!reason175_.first_seen)
                {
                    reason175_.observe_request(observation.tid, reason, frame_valid ? rsp + 0x450 : 0, wrapper_caller, now,
                                               observation.token);
                    observation.flags |= earlier_unobserved;
                }
                if (reason175_.observe_setter(observation.tid, target, reason, manager, frame_valid ? rsp + 0x450 : 0, wrapper_caller,
                                              frame_valid))
                {
                    observation.flags |= reason175_setter_verified;
                }
                else
                {
                    observation.flags |= reason175_ambiguous;
                }
                break;
            }
            case 3: {
                const auto manager = reg(x86_register::rbx);
                const auto previous = static_cast<uint32_t>(reg(x86_register::r15));
                const auto timestamp_register = reg(x86_register::rax);
                uint64_t caller{};
                const auto state_read = sampled(manager, 0x390, observation.bytes.data(), 24, 1);
                sampled(rsp, 0x328, &caller, sizeof(caller), 2);
                observation.length = state_read ? 24 : 0;
                const auto current = field<uint32_t>(observation.bytes, 0);
                const auto reason = field<uint32_t>(observation.bytes, 4);
                const auto timestamp = field<uint64_t>(observation.bytes, 8);
                const auto goal = field<uint32_t>(observation.bytes, 16);
                const auto goal_reason = field<uint32_t>(observation.bytes, 20);
                observation.values = {manager,
                                      previous,
                                      current,
                                      reason,
                                      timestamp,
                                      goal,
                                      goal_reason,
                                      timestamp_register,
                                      caller,
                                      static_cast<uint32_t>(reg(x86_register::eflags)),
                                      cleanup_guards_verified_,
                                      mask};
                if (destiny_reason175_capture_state::read_complete(mask, 3) && (current != 28 || goal != 28))
                {
                    emit = false;
                    break;
                }
                const auto frame_valid = destiny_reason175_capture_state::read_complete(mask, 3) && cleanup_guards_verified_ &&
                                         caller == base_ + 0xE23568 && timestamp == timestamp_register && current == 28 && goal == 28 &&
                                         previous != 28 && reason == goal_reason;
                if (frame_valid && !reason175_.first_seen)
                {
                    reason175_.observe_request(observation.tid, reason, 0, 0, now, observation.token);
                    observation.flags |= earlier_unobserved;
                }
                if (reason175_.observe_commit(manager, current, reason, goal, goal_reason, frame_valid))
                {
                    observation.flags |= reason175_commit_verified;
                }
                else
                {
                    observation.flags |= reason175_ambiguous;
                }
                terminal = frame_valid;
                break;
            }
            default:
                emit = false;
                break;
            }
        }
        catch (...)
        {
            observation.flags |= read_failed;
        }
        if (observation.flags & read_failed)
        {
            reason175_.ambiguous = true;
            observation.flags |= reason175_ambiguous;
        }
        state.identity_misses += (observation.flags & identity_miss) ? 1 : 0;
        state.read_failures += (observation.flags & read_failed) ? 1 : 0;
        if (emit && enqueue_locked(observation))
        {
            ++state.emitted;
        }
        if (emit_detail && enqueue_locked(detail))
        {
            ++state.emitted;
        }
        const auto elapsed =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
        state.callback_ns += elapsed;
        state.callback_max_ns = std::max(state.callback_max_ns, elapsed);
        uint64_t callback_total{};
        for (size_t index = 0; index < selected_site_count(); ++index)
        {
            callback_total += sites_[index].callback_ns;
        }
        if (!retired_ && terminal)
        {
            retire_locked(positive_gate, now);
        }
        else if (!retired_ && destiny_reason175_capture_state::callback_exhausted(state.hits, callback_total) &&
                 state.hits >= destiny_reason175_capture_state::hit_limit)
        {
            retire_locked(hit_budget, now);
        }
        else if (!retired_ && destiny_reason175_capture_state::callback_exhausted(state.hits, callback_total))
        {
            retire_locked(callback_budget, now);
        }
        if (retired_)
        {
            remove_hooks_locked(cpu.index());
        }
    }

    bool destiny_reason175_capture_state::dependency_matches(const size_t dependency, const uint8_t* data, const size_t length)
    {
        if (dependency >= dependencies.size() || !data || length != dependencies[dependency].length)
        {
            return false;
        }
        return std::equal(data, data + length, dependencies[dependency].bytes.begin());
    }

    bool destiny_reason175_capture_state::callback_exhausted(const uint64_t hits, const uint64_t callback_ns)
    {
        return hits >= hit_limit || callback_ns >= callback_limit_ns;
    }

    bool destiny_reason175_capture_state::guard_matches(const size_t site, const uint8_t* data, const size_t length)
    {
        if (site >= site_count || !data || length != lengths[site])
        {
            return false;
        }
        for (size_t index = 0; index < length; ++index)
        {
            if (data[index] != bytes[site][index])
            {
                return false;
            }
        }
        return true;
    }

    bool destiny_reason175_capture_state::quiescent(const bool* running, const size_t count, const size_t executing)
    {
        if (!running && count)
        {
            return false;
        }
        for (size_t index = 0; index < count; ++index)
        {
            if (index != executing && running[index])
            {
                return false;
            }
        }
        return true;
    }

    bool destiny_reason175_capture_state::read_complete(const uint64_t mask, const uint64_t required)
    {
        return (mask & required) == required;
    }

    bool destiny_reason175_capture_state::record_allowed(const size_t accepted, const size_t pending, const bool coverage)
    {
        return accepted < max_records - (coverage ? 0 : coverage_reserve) && pending < max_records;
    }

    bool destiny_reason175_capture_state::output_allowed(const size_t records, const size_t output_bytes, const size_t charged_bytes,
                                                         const bool coverage)
    {
        const auto limit = max_bytes - (coverage ? 0 : coverage_reserve * output_line_bytes);
        return records < max_records && charged_bytes <= limit && output_bytes <= limit - charged_bytes;
    }

    void destiny_reason175_capture_state::arm(const uint64_t now)
    {
        if (!armed)
        {
            armed = true;
            armed_at = now;
        }
    }

    std::string_view destiny_reason175_capture_state::expiry(const uint64_t now) const
    {
        if (armed && now >= armed_at && now - armed_at >= total_ms)
        {
            return "total_budget";
        }
        if (first_seen && now >= first_at && now - first_at >= closure_ms)
        {
            return "first_request_closure_budget";
        }
        return {};
    }

    bool destiny_reason175_capture_state::observe_request(const uint32_t observed_tid, const uint32_t observed_reason, const uint64_t rsp,
                                                          const uint64_t caller, const uint64_t now, const uint64_t sequence)
    {
        if (first_seen)
        {
            ambiguous = true;
            return false;
        }
        first_seen = true;
        first_at = now;
        tid = observed_tid;
        reason = observed_reason;
        request_rsp = rsp;
        request_caller = caller;
        first_sequence = sequence;
        return true;
    }

    bool destiny_reason175_capture_state::observe_setter(const uint32_t observed_tid, const uint32_t target, const uint32_t observed_reason,
                                                         const uint64_t observed_manager, const uint64_t wrapper_rsp,
                                                         const uint64_t wrapper_caller, const bool frame_valid)
    {
        if (target != 28)
        {
            return false;
        }
        if (!first_seen || setter_seen || !frame_valid || !observed_tid || !tid || !request_rsp || !request_caller || observed_tid != tid ||
            observed_reason != reason || wrapper_rsp != request_rsp || wrapper_caller != request_caller || !observed_manager)
        {
            ambiguous = true;
            return false;
        }
        manager = observed_manager;
        setter_seen = true;
        return true;
    }

    bool destiny_reason175_capture_state::observe_commit(const uint64_t observed_manager, const uint32_t current,
                                                         const uint32_t current_reason, const uint32_t goal, const uint32_t goal_reason,
                                                         const bool frame_valid)
    {
        if (current != 28 || goal != 28)
        {
            return false;
        }
        if (!first_seen || !setter_seen || commit_seen || !frame_valid || observed_manager != manager || current_reason != reason ||
            goal_reason != reason)
        {
            ambiguous = true;
            return false;
        }
        commit_seen = true;
        return true;
    }

    bool destiny_reason175_capture_state::complete(const uint64_t drops, const uint64_t failed_reads) const
    {
        return first_seen && producer_verified && setter_seen && commit_seen && !ambiguous && !drops && !failed_reads;
    }

    bool destiny_startup_capture::reason175_enabled()
    {
        const auto* value = std::getenv("SOGEN_DESTINY_REASON175_CAPTURE");
        return value && std::strcmp(value, "1") == 0;
    }

    size_t destiny_startup_capture::selected_site_count() const
    {
        return focused_ ? destiny_reason175_capture_state::site_count : sites_.size();
    }

    size_t destiny_startup_capture::record_limit() const
    {
        return focused_ ? destiny_reason175_capture_state::max_records : destiny_startup_capture_budget::max_records;
    }

    size_t destiny_startup_capture::byte_limit() const
    {
        return focused_ ? destiny_reason175_capture_state::max_bytes : destiny_startup_capture_budget::max_bytes;
    }

    size_t destiny_startup_capture::coverage_reserve() const
    {
        return focused_ ? destiny_reason175_capture_state::coverage_reserve : destiny_startup_capture_budget::coverage_reserve;
    }

    std::string_view destiny_startup_capture::capture_expiry(const uint64_t now) const
    {
        return focused_ ? reason175_.expiry(now) : budget_.expiry(now);
    }

    void destiny_startup_capture_budget::arm(const uint64_t now)
    {
        if (!armed)
        {
            armed = true;
            armed_at = now;
        }
    }

    void destiny_startup_capture_budget::observe_cleanup(const uint64_t now)
    {
        if (armed && !cleanup_seen)
        {
            cleanup_seen = true;
            cleanup_at = now;
        }
    }

    std::string_view destiny_startup_capture_budget::expiry(const uint64_t now) const
    {
        const auto cleanup_expired = armed && cleanup_seen && now >= cleanup_at && now - cleanup_at >= post_cleanup_ms;
        if (cleanup_expired && cleanup_at >= armed_at && cleanup_at - armed_at <= total_ms - post_cleanup_ms)
        {
            return "post_cleanup_budget";
        }
        if (armed && now >= armed_at && now - armed_at >= total_ms)
        {
            return "total_budget";
        }
        if (cleanup_expired)
        {
            return "post_cleanup_budget";
        }
        return {};
    }

    bool destiny_startup_capture::enabled()
    {
        const auto* value = std::getenv("SOGEN_DESTINY_STARTUP_CAPTURE");
        return reason175_enabled() || (value && std::strcmp(value, "1") == 0);
    }

    std::shared_ptr<destiny_startup_capture> destiny_startup_capture::create(windows_emulator& owner, kernel_lock& kernel,
                                                                             const uint64_t base, const uint64_t image_size)
    {
        if (base < 0x10000 || base >= user_end || !image_size || image_size > user_end - base)
        {
            owner.log.warn("DESTINYSTARTUP unavailable reason=module_bounds\n");
            return {};
        }
        auto capture = std::shared_ptr<destiny_startup_capture>(new destiny_startup_capture(owner, kernel, base, image_size));
        capture->start();
        return capture;
    }

    destiny_startup_capture::destiny_startup_capture(windows_emulator& owner, kernel_lock& kernel, const uint64_t base,
                                                     const uint64_t image_size)
        : owner_(owner),
          kernel_(kernel),
          base_(base),
          image_size_(image_size)
    {
    }

    destiny_startup_capture::~destiny_startup_capture()
    {
        shutdown();
    }

    uint64_t destiny_startup_capture::now_ms() const
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - origin_).count());
    }

    void destiny_startup_capture::start()
    {
        const std::weak_ptr<destiny_startup_capture> weak = shared_from_this();
        owner_.callbacks.on_debug_string.add([weak](const std::string_view message) {
            if (const auto self = weak.lock())
            {
                self->observe_debug(message);
            }
        });
        owner_.callbacks.on_module_unload.add([weak](mapped_module& module) {
            if (const auto self = weak.lock())
            {
                self->observe_unload(module);
            }
        });
        output_worker_ = std::jthread([this](const std::stop_token& stop) { output_worker(stop); });
        worker_ = std::jthread([this](const std::stop_token& stop) { maintenance(stop); });
    }

    bool destiny_startup_capture::read(const uint64_t address, void* destination, const size_t length, record& observation)
    {
        ++read_requests_;
        try
        {
            if (user_range(address, length) && owner_.emu().try_read_memory(address, destination, length))
            {
                return true;
            }
        }
        catch (...)
        {
        }
        observation.flags |= read_failed;
        ++failed_read_requests_;
        return false;
    }

    bool destiny_startup_capture::read_offset(const uint64_t address, const uint64_t offset, void* destination, const size_t length,
                                              record& observation)
    {
        if (!user_range(address, 1) || offset >= user_end - address)
        {
            ++read_requests_;
            ++failed_read_requests_;
            observation.flags |= read_failed;
            return false;
        }
        return read(address + offset, destination, length, observation);
    }

    bool destiny_startup_capture::enqueue_locked(record observation, const bool is_coverage)
    {
        const auto limit = record_limit() - (is_coverage ? 0 : coverage_reserve());
        if ((focused_ && !destiny_reason175_capture_state::record_allowed(accepted_, pending_count_, is_coverage)) ||
            (!focused_ && (accepted_ >= limit || pending_count_ >= records_.size())))
        {
            if (focused_)
            {
                reason175_.ambiguous = true;
            }
            ++dropped_;
            if (observation.kind == gate && observation.site < sites_.size())
            {
                ++sites_[observation.site].dropped;
            }
            if (!retired_ && !is_coverage)
            {
                retire_locked(record_budget, now_ms());
            }
            return false;
        }
        records_[(pending_begin_ + pending_count_) % records_.size()] = observation;
        ++pending_count_;
        ++accepted_;
        wake_.notify_all();
        return true;
    }

    void destiny_startup_capture::baseline_locked(const uint8_t phase, const uint64_t now)
    {
        if (snapshots_ >= 64 || (snapshot_seen_ && now >= last_snapshot_ && now - last_snapshot_ < 1000))
        {
            ++snapshot_misses_;
            return;
        }
        snapshot_seen_ = true;
        last_snapshot_ = now;
        ++snapshots_;
        for (size_t index = 0; index != resources_.size(); ++index)
        {
            record observation{};
            observation.kind = baseline;
            observation.recorder = static_cast<uint8_t>(index);
            observation.host_ms = now;
            observation.flags = native_generation_unavailable | earlier_unobserved;
            const auto slot = base_ + resource_rva + (index ? 6 : 3) * 12;
            std::array<uint8_t, 12> bytes{};
            uint32_t outstanding{};
            if (resource_rva + 0x64 <= image_size_ && read(slot, bytes.data(), bytes.size(), observation) &&
                read(base_ + resource_rva + 0x60, &outstanding, sizeof(outstanding), observation))
            {
                auto& resource = resources_[index];
                const auto handle = field<uint32_t>(bytes, 4);
                if (!resource.pre_free)
                {
                    if (resource.handle != handle)
                    {
                        resource.token = ++serial_;
                        resource.native_assignment = false;
                    }
                    resource.handle = handle;
                    resource.tag = field<uint32_t>(bytes, 8);
                    resource.slot = slot;
                }
                observation.token = resource.token;
                observation.values = {
                    phase, static_cast<uint64_t>(index ? 6 : 3), bytes[0], bytes[1], handle, field<uint32_t>(bytes, 8), outstanding};
                observation.bytes[0] = bytes[0];
            }
            enqueue_locked(observation);
        }
        record registry{};
        registry.kind = baseline;
        registry.recorder = 2;
        registry.host_ms = now;
        registry.values[0] = phase;
        uint64_t current{};
        uint64_t registry_object{};
        uint64_t vtable{};
        uint32_t mode{};
        uint32_t content{};
        if (0x2742FB8 <= image_size_ && read(base_ + 0x2742FA0, &current, sizeof(current), registry) &&
            read(base_ + 0x2742FB0, &registry_object, sizeof(registry_object), registry))
        {
            registry.values[1] = current;
            registry.values[2] = registry_object;
            if (registry_object && read(registry_object, &vtable, sizeof(vtable), registry))
            {
                registry.values[3] = vtable;
                if (vtable == base_ + 0x1BEB568)
                {
                    read_offset(registry_object, 8, &mode, sizeof(mode), registry);
                    read_offset(registry_object, 12, &content, sizeof(content), registry);
                    registry.values[4] = mode;
                    registry.values[5] = content;
                }
                else
                {
                    registry.flags |= identity_miss;
                }
            }
        }
        enqueue_locked(registry);
    }

    void destiny_startup_capture::observe_debug(const std::string_view message)
    {
        constexpr std::string_view entry = "world_controller:state_manager: Entering state 'bootflow:";
        if (message.find(entry) == std::string_view::npos)
        {
            return;
        }
        std::unique_lock kernel_guard(kernel_, std::defer_lock);
        if (!kernel_.is_held_by_current_thread())
        {
            kernel_guard.lock();
        }
        const std::scoped_lock state_guard(mutex_);
        if (retired_ || shutdown_)
        {
            return;
        }
        const auto now = now_ms();
        const auto investment_entry = message.find("bootflow:investment_signin'") != std::string_view::npos;
        if (focused_)
        {
            if (!reason175_.armed && investment_entry)
            {
                budget_.arm(now);
                reason175_.arm(now);
                arm_reason175_locked(now);
            }
            return;
        }
        if (!baseline_seen_)
        {
            baseline_seen_ = true;
            if (!investment_entry)
            {
                baseline_locked(0, now);
            }
        }
        if (!budget_.armed && investment_entry)
        {
            budget_.arm(now);
            baseline_locked(1, now);
            arm_locked(now);
        }
    }

    bool destiny_startup_capture::verify_cleanup_guards_locked(const uint64_t now)
    {
        constexpr std::array<uint8_t, 35> prologue{0x40, 0x55, 0x53, 0x48, 0x8D, 0xAC, 0x24, 0xE8, 0xFD, 0xFF, 0xFF, 0x48,
                                                   0x81, 0xEC, 0x18, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xFF, 0xEB, 0x28,
                                                   0x01, 0x48, 0x33, 0xC4, 0x48, 0x89, 0x85, 0xF0, 0x01, 0x00, 0x00};
        constexpr std::array<uint8_t, 7> previous_state{0x44, 0x8B, 0xBB, 0x90, 0x03, 0x00, 0x00};
        constexpr std::array<uint8_t, 65> commit{0x8B, 0x83, 0xA0, 0x03, 0x00, 0x00, 0x89, 0x83, 0x90, 0x03, 0x00, 0x00, 0x8B,
                                                 0x83, 0xA4, 0x03, 0x00, 0x00, 0x89, 0x83, 0x94, 0x03, 0x00, 0x00, 0x80, 0x3D,
                                                 0x6D, 0x88, 0x89, 0x01, 0x00, 0x48, 0x8B, 0x05, 0x6E, 0x88, 0x89, 0x01, 0x75,
                                                 0x05, 0xE8, 0x8F, 0x35, 0x4E, 0xFF, 0x83, 0xBB, 0xA0, 0x03, 0x00, 0x00, 0xFF,
                                                 0x48, 0x89, 0x83, 0x98, 0x03, 0x00, 0x00, 0x0F, 0x84, 0x11, 0x01, 0x00, 0x00};
        constexpr std::array<uint8_t, 15> caller{0x48, 0x8B, 0xCB, 0xE8, 0x08, 0x79, 0xFF, 0xFF, 0x48, 0x63, 0x83, 0x90, 0x03, 0x00, 0x00};
        constexpr std::array<uint8_t, 6> instruction{0x0F, 0x84, 0x11, 0x01, 0x00, 0x00};
        const auto verify = [&](const uint64_t rva, const auto& expected) {
            std::array<uint8_t, 65> actual{};
            record check{};
            check.kind = signature;
            check.site = 252;
            check.recorder = 3;
            check.host_ms = now;
            check.rip = base_ + rva;
            const auto readable =
                rva <= image_size_ && expected.size() <= image_size_ - rva && read(check.rip, actual.data(), expected.size(), check);
            const auto verified = readable && std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
            if (!readable)
            {
                ++sites_[site_count - 1].read_failures;
            }
            check.flags |= verified ? matched : identity_miss;
            check.values = {rva, expected.size(), 0, verified, readable};
            check.length = static_cast<uint8_t>(std::min(expected.size(), check.bytes.size()));
            std::copy_n(actual.begin(), check.length, check.bytes.begin());
            enqueue_locked(check);
            if (expected.size() > check.bytes.size())
            {
                check.values[2] = check.bytes.size();
                check.length = static_cast<uint8_t>(expected.size() - check.bytes.size());
                check.bytes = {};
                std::copy_n(actual.begin() + 64, check.length, check.bytes.begin());
                enqueue_locked(check);
            }
            return verified;
        };
        auto verified = verify(0xE1AE70, prologue);
        verified = verify(0xE1AEF9, previous_state) && verified;
        verified = verify(0xE1B094, commit) && verified;
        verified = verify(0xE23560, caller) && verified;
        verified = verify(0xE1B0CF, instruction) && verified;
        record availability{};
        availability.kind = unavailable;
        availability.site = 252;
        availability.recorder = 3;
        availability.host_ms = now;
        availability.flags = verified ? matched : identity_miss | earlier_unobserved;
        availability.values = {verified,
                               5,
                               0xE1B0CF,
                               0xE23568,
                               0x328,
                               focused_ ? destiny_reason175_capture_state::closure_ms : destiny_startup_capture_budget::post_cleanup_ms,
                               focused_ ? destiny_reason175_capture_state::total_ms : destiny_startup_capture_budget::total_ms};
        enqueue_locked(availability);
        return verified;
    }

    void destiny_startup_capture::arm_locked(const uint64_t now)
    {
        record receipt{};
        receipt.kind = activation;
        receipt.recorder = 3;
        receipt.host_ms = now;
        receipt.values = {base_,
                          image_size_,
                          destiny_startup_capture_budget::total_ms,
                          destiny_startup_capture_budget::post_cleanup_ms,
                          destiny_startup_capture_budget::max_records,
                          destiny_startup_capture_budget::max_bytes,
                          128,
                          2000000000};
        enqueue_locked(receipt);
        static_assert(sites.size() == site_count);
        static_assert(destiny_startup_capture_budget::coverage_reserve >= 2 * site_count + 5);
        cleanup_guards_verified_ = verify_cleanup_guards_locked(now);
        const std::weak_ptr<destiny_startup_capture> weak = shared_from_this();
        for (size_t index = 0; index < sites_.size(); ++index)
        {
            auto& state = sites_[index];
            const auto& site = sites[index];
            state.attempted = true;
            record check{};
            check.kind = signature;
            check.site = static_cast<uint8_t>(index);
            check.recorder = 3;
            check.host_ms = now;
            check.rip = base_ + site.rva;
            check.length = static_cast<uint8_t>(site.length);
            const auto valid =
                site.rva <= image_size_ && site.length <= image_size_ - site.rva && read(check.rip, check.bytes.data(), site.length, check);
            if (!valid || std::memcmp(check.bytes.data(), site.bytes.data(), site.length) != 0 ||
                (index == site_count - 1 && !cleanup_guards_verified_))
            {
                ++state.signature_misses;
                if (!valid)
                {
                    ++state.read_failures;
                }
                check.flags |= identity_miss;
                enqueue_locked(check);
                continue;
            }
            check.flags |= matched;
            enqueue_locked(check);
            try
            {
                state.hook = owner_.emu().hook_memory_execution_with_mode(check.rip, hook_interface::memory_execution_hook_mode::int3,
                                                                          [weak, index](cpu_interface& cpu, const uint64_t rip) {
                                                                              if (const auto self = weak.lock())
                                                                              {
                                                                                  self->capture(cpu, rip, index);
                                                                              }
                                                                          });
            }
            catch (...)
            {
                state.retirement = install_failure;
                record failure = check;
                failure.kind = unavailable;
                failure.values[0] = install_failure;
                enqueue_locked(failure);
            }
        }
        std::array<uint8_t, dispatcher_prologue.size()> actual{};
        record frame{};
        frame.kind = unavailable;
        frame.site = 254;
        frame.recorder = 2;
        frame.host_ms = now;
        constexpr std::array<uint8_t, 9> sender_prologue{0x40, 0x53, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00};
        constexpr std::array<uint8_t, 4> payload_prologue{0x48, 0x83, 0xEC, 0x78};
        std::array<uint8_t, sender_prologue.size()> sender_actual{};
        std::array<uint8_t, payload_prologue.size()> payload_actual{};
        dispatcher_frame_verified_ = 0xF2C280 + actual.size() <= image_size_ &&
                                     read(base_ + 0xF2C280, actual.data(), actual.size(), frame) && actual == dispatcher_prologue;
        dispatcher_frame_verified_ =
            dispatcher_frame_verified_ && read(base_ + 0xBE7E00, sender_actual.data(), sender_actual.size(), frame) &&
            sender_actual == sender_prologue && read(base_ + 0xF2BCC0, payload_actual.data(), payload_actual.size(), frame) &&
            payload_actual == payload_prologue;
        frame.values[0] = dispatcher_frame_verified_;
        frame.values[1] = 0xF2C280;
        frame.values[2] = 0x458;
        frame.values[3] = 0x460;
        frame.values[4] = 1;
        frame.length = static_cast<uint8_t>(actual.size());
        std::ranges::copy(actual, frame.bytes.begin());
        enqueue_locked(frame);
        constexpr std::array<uint8_t, 69> pool_guard{0x41, 0x8B, 0xC0, 0x41, 0x81, 0xE0, 0xFF, 0x1F, 0x00, 0x00, 0xC1, 0xF8, 0x0D, 0x8B,
                                                     0xD0, 0x48, 0x81, 0xCA, 0x00, 0x00, 0xFC, 0x0F, 0x0F, 0xB7, 0xC0, 0x48, 0xC1, 0xEA,
                                                     0x12, 0x48, 0x23, 0xD0, 0x48, 0x8B, 0x05, 0xE0, 0xD5, 0x00, 0x02, 0x48, 0xC1, 0xE2,
                                                     0x06, 0x48, 0x03, 0x10, 0x44, 0x0F, 0xAF, 0x42, 0x30, 0x48, 0x63, 0x42, 0x34, 0x41,
                                                     0x8B, 0xC8, 0x48, 0x03, 0x4A, 0x08, 0x48, 0x23, 0x41, 0x08, 0x48, 0x2B, 0xC8};
        std::array<uint8_t, pool_guard.size()> pool_actual{};
        record layout{};
        layout.kind = unavailable;
        layout.recorder = 3;
        layout.site = 254;
        layout.host_ms = now;
        pool_layout_verified_ = 0x42C669 + pool_actual.size() <= image_size_ &&
                                read(base_ + 0x42C669, pool_actual.data(), pool_actual.size(), layout) && pool_actual == pool_guard;
        layout.values = {pool_layout_verified_, 0x42C669, pool_guard.size()};
        layout.length = 64;
        std::copy_n(pool_actual.begin(), 64, layout.bytes.begin());
        enqueue_locked(layout);
        layout.values[3] = 64;
        layout.length = static_cast<uint8_t>(pool_actual.size() - 64);
        layout.bytes = {};
        std::copy(pool_actual.begin() + 64, pool_actual.end(), layout.bytes.begin());
        enqueue_locked(layout);
        record limitations{};
        limitations.kind = unavailable;
        limitations.recorder = 3;
        limitations.site = 255;
        limitations.host_ms = now;
        limitations.values = {2, 0, 0, 0};
        limitations.flags = native_generation_unavailable | earlier_unobserved;
        enqueue_locked(limitations);
    }

    bool destiny_startup_capture::resolve_request(const uint32_t handle, request_view& view, record& observation)
    {
        if (!pool_layout_verified_ || handle == 0xFFFFFFFF || 0x2439C78 > image_size_)
        {
            return false;
        }
        const auto quotient = static_cast<uint32_t>(static_cast<int32_t>(handle) >> 13);
        const auto index = (quotient & 0xFFFF) & ((quotient | 0x0FFC0000) >> 18);
        if (index > 0x3FFF)
        {
            return false;
        }
        uint64_t directory{};
        uint64_t table{};
        if (!read(base_ + 0x2439C70, &directory, 8, observation) || !read(directory, &table, 8, observation))
        {
            return false;
        }
        if (!user_range(table, 1) || static_cast<uint64_t>(index) * 0x40 >= user_end - table)
        {
            return false;
        }
        const auto descriptor_address = table + static_cast<uint64_t>(index) * 0x40;
        std::array<uint8_t, 0x38> descriptor{};
        if (!read(descriptor_address, descriptor.data(), descriptor.size(), observation))
        {
            return false;
        }
        const auto data_base = field<uint64_t>(descriptor, 8);
        const auto stride = field<uint32_t>(descriptor, 0x30);
        if (stride < 16 || stride > 0x10000)
        {
            return false;
        }
        const auto product = static_cast<uint32_t>((handle & 0x1FFF) * stride);
        if (!user_range(data_base, 1) || product >= user_end - data_base)
        {
            return false;
        }
        const auto slot = data_base + product;
        uint64_t encoded{};
        if (!read(slot + 8, &encoded, 8, observation))
        {
            return false;
        }
        const auto mask = static_cast<uint64_t>(static_cast<int64_t>(field<int32_t>(descriptor, 0x34)));
        const auto indirection = encoded & mask;
        if (slot < indirection)
        {
            return false;
        }
        view.address = slot - indirection;
        if (!read(view.address, view.bytes.data(), view.bytes.size(), observation))
        {
            return false;
        }
        uint64_t directory_check{};
        uint64_t table_check{};
        uint64_t encoded_check{};
        std::array<uint8_t, 0x38> descriptor_check{};
        std::array<uint8_t, 10> request_check{};
        return read(base_ + 0x2439C70, &directory_check, 8, observation) && directory_check == directory &&
               read(directory, &table_check, 8, observation) && table_check == table &&
               read(descriptor_address, descriptor_check.data(), descriptor_check.size(), observation) && descriptor_check == descriptor &&
               read(slot + 8, &encoded_check, 8, observation) && encoded_check == encoded &&
               read(view.address + 6, request_check.data(), request_check.size(), observation) &&
               std::equal(request_check.begin(), request_check.end(), view.bytes.begin() + 6);
    }

    destiny_startup_capture::sender_state* destiny_startup_capture::sender_locked(const uint32_t tid, const bool allocate)
    {
        if (!tid)
        {
            return nullptr;
        }
        for (auto& sender : senders_)
        {
            if (sender.tid == tid)
            {
                return &sender;
            }
        }
        if (allocate)
        {
            for (auto& sender : senders_)
            {
                if (!sender.tid)
                {
                    sender.tid = tid;
                    return &sender;
                }
            }
        }
        return nullptr;
    }

    void destiny_startup_capture::capture(cpu_interface& cpu, const uint64_t rip, const size_t site_index)
    {
        if (focused_)
        {
            capture_reason175(cpu, rip, site_index);
            return;
        }
        const auto begin = std::chrono::steady_clock::now();
        sites_[site_index].raw_traps.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock kernel_guard(kernel_, std::defer_lock);
        if (!kernel_.is_held_by_current_thread())
        {
            kernel_guard.lock();
        }
        const std::scoped_lock state_guard(mutex_);
        auto& state = sites_[site_index];
        if (retired_ || shutdown_ || state.retirement)
        {
            ++retired_traps_;
            remove_hooks_locked(cpu.index());
            return;
        }
        if (!state.hook)
        {
            return;
        }
        ++state.hits;
        record observation{};
        observation.kind = gate;
        observation.site = static_cast<uint8_t>(site_index);
        observation.recorder = 2;
        observation.host_ms = now_ms();
        observation.rip = rip;
        observation.vcpu = static_cast<uint32_t>(cpu.index());
        const auto expired = budget_.expiry(observation.host_ms);
        if (!expired.empty())
        {
            retire_locked(expired == "total_budget" ? total_budget : cleanup_budget, observation.host_ms);
            remove_hooks_locked(cpu.index());
            return;
        }
        try
        {
            auto& vcpu = owner_.vcpu(cpu.index());
            auto& acting = vcpu.cpu;
            observation.tid = vcpu.active_thread ? vcpu.active_thread->id : 0;
            const auto reg = [&](const x86_register name) { return acting.reg<uint64_t>(name); };
            const auto rcx = reg(x86_register::rcx);
            const auto rdx = reg(x86_register::rdx);
            const auto rax = reg(x86_register::rax);
            const auto rbx = reg(x86_register::rbx);
            const auto rbp = reg(x86_register::rbp);
            const auto rdi = reg(x86_register::rdi);
            const auto rsi = reg(x86_register::rsi);
            const auto r8 = reg(x86_register::r8);
            const auto r9 = reg(x86_register::r9);
            const auto r14 = reg(x86_register::r14);
            const auto r15 = reg(x86_register::r15);
            const auto rsp = reg(x86_register::rsp);
            auto* sender = sender_locked(observation.tid, site_index == 22);
            const auto sender_frame = [&] {
                uint64_t return_address{};
                return dispatcher_frame_verified_ && sender && sender->stage && rsp <= sender->pump_rsp && sender->pump_rsp - rsp == 0xA0 &&
                       read_offset(rsp, 0x98, &return_address, 8, observation) && return_address == base_ + 0xBE95C7 &&
                       rbx == sender->record_address;
            };
            const auto peer_frame = [&] {
                uint64_t return_address{};
                return dispatcher_frame_verified_ && sender && sender->stage == 5 && rsp <= sender->dispatch_rsp &&
                       sender->dispatch_rsp - rsp == 0x460 && rbp <= user_end - 0x360 && rbp + 0x360 == sender->dispatch_rsp &&
                       read_offset(rsp, 0x458, &return_address, 8, observation) && return_address == base_ + 0xF2BD0C &&
                       (rdi & 0xFFFF) == 503 && rsi == 0 && static_cast<uint32_t>(r14) == sender->dispatch_mode &&
                       r15 == sender->descriptor && rbx == sender->completion_token;
            };
            const auto capture_primary = [&](const uint64_t manager) {
                observation.values[0] = manager;
                if (read(manager, observation.bytes.data(), 48, observation))
                {
                    observation.length = 48;
                    observation.values[1] = observation.bytes[0];
                    observation.values[2] = field<uint64_t>(observation.bytes, 1);
                }
            };
            const auto registry = [&](const uint64_t object, const size_t content_index, const size_t mode_index,
                                      const size_t vtable_index) {
                uint64_t vtable{};
                uint32_t content{};
                uint32_t mode{};
                observation.values[0] = object;
                if (read(object, &vtable, 8, observation) && vtable == base_ + 0x1BEB568)
                {
                    read_offset(object, 8, &mode, 4, observation);
                    read_offset(object, 12, &content, 4, observation);
                    observation.values[content_index] = content;
                    observation.values[mode_index] = mode;
                    observation.values[vtable_index] = vtable;
                }
                else
                {
                    observation.flags |= identity_miss;
                }
            };
            const auto resource_event = site_index <= 3 || site_index == 33;
            if (resource_event)
            {
                const auto index = site_index == 3 ? static_cast<uint32_t>(rdx) : static_cast<uint32_t>(rbp);
                const auto loader = site_index == 3 ? rcx : r14;
                const auto slot = base_ + resource_rva + static_cast<uint64_t>(index) * 12;
                if ((index != 3 && index != 6) || loader != base_ + resource_rva || (site_index != 3 && r15 != slot))
                {
                    observation.flags |= identity_miss;
                }
                else
                {
                    const auto resource_index = index == 6 ? 1U : 0U;
                    auto& resource = resources_[resource_index];
                    if (site_index == 0)
                    {
                        resource.pre_free = false;
                    }
                    observation.recorder = static_cast<uint8_t>(resource_index);
                    std::array<uint8_t, 12> slot_bytes{};
                    uint32_t outstanding{};
                    if (read(slot, slot_bytes.data(), slot_bytes.size(), observation) &&
                        read(base_ + resource_rva + 0x60, &outstanding, 4, observation))
                    {
                        const auto handle = field<uint32_t>(slot_bytes, 4);
                        const auto tag = field<uint32_t>(slot_bytes, 8);
                        if (site_index == 0)
                        {
                            request_view wrapper{};
                            request_view child{};
                            wrapper.address = rdi;
                            if (handle == static_cast<uint32_t>(rdx) && handle != 0xFFFFFFFF &&
                                read(rdi, wrapper.bytes.data(), wrapper.bytes.size(), observation))
                            {
                                if (resource.handle != handle)
                                {
                                    resource.token = ++serial_;
                                    resource.native_assignment = false;
                                }
                                resource.handle = handle;
                                resource.child = 0xFFFFFFFF;
                                if (wrapper.bytes[6] == 2 && resolve_request(field<uint32_t>(wrapper.bytes, 12), child, observation) &&
                                    child.bytes[6] == 3 && field<uint32_t>(child.bytes, 12) == handle)
                                {
                                    resource.child = field<uint32_t>(wrapper.bytes, 12);
                                }
                                else
                                {
                                    observation.flags |= earlier_unobserved;
                                }
                                resource.wrapper = wrapper.address;
                                resource.wrapper_bytes = wrapper.bytes;
                                resource.tag = tag;
                                resource.slot = slot;
                                resource.owner = rcx;
                                resource.tid = observation.tid;
                                resource.terminal_status = static_cast<uint32_t>(rbx);
                                resource.pre_free_rsp = rsp;
                                resource.pre_free = observation.tid != 0;
                                observation.flags |= matched | pre_release | native_generation_unavailable;
                                std::copy_n(wrapper.bytes.begin(), 64, observation.bytes.begin());
                                observation.length = 64;
                            }
                            else
                            {
                                observation.flags |= identity_miss;
                            }
                        }
                        else if (!resource.pre_free || resource.slot != slot || resource.tid != observation.tid ||
                                 resource.pre_free_rsp != rsp || resource.terminal_status != static_cast<uint32_t>(rbx) ||
                                 (site_index != 3 && handle != 0xFFFFFFFF))
                        {
                            observation.flags |= identity_miss;
                        }
                        else
                        {
                            observation.flags |= matched | native_generation_unavailable;
                        }
                        if (static_cast<uint32_t>(rbx) == 3)
                        {
                            observation.flags |= terminal_status_three;
                        }
                        observation.token = resource.token;
                        observation.values = {resource.handle,
                                              index,
                                              static_cast<uint32_t>(rbx),
                                              tag,
                                              slot_bytes[0],
                                              slot_bytes[1],
                                              outstanding,
                                              resource.wrapper,
                                              resource.child,
                                              resource.owner};
                    }
                }
            }
            else if (site_index == 5 || site_index == 36 || site_index == 37)
            {
                const auto owner = site_index == 5 ? r15 : rbp;
                const auto context = site_index == 5 ? rbx : r14;
                auto active_child = static_cast<uint32_t>(rbx);
                auto transaction = static_cast<uint16_t>(rax);
                bool native_inputs = true;
                if (site_index == 5)
                {
                    active_child = 0xFFFFFFFF;
                    transaction = 0xFFFF;
                    native_inputs =
                        read_offset(owner, 0x220068, &active_child, 4, observation) && read(context, &transaction, 2, observation);
                }
                else if (site_index == 37)
                {
                    native_inputs = read(context, &transaction, 2, observation);
                }
                observation.values = {0xFFFFFFFF, active_child, transaction, static_cast<uint32_t>(rax), 0, 0, context, owner};
                bool found{};
                for (size_t resource_index = 0; resource_index != resources_.size(); ++resource_index)
                {
                    if (!native_inputs)
                    {
                        break;
                    }
                    auto& resource = resources_[resource_index];
                    std::array<uint8_t, 12> slot_bytes{};
                    request_view wrapper{};
                    request_view child{};
                    const auto slot = base_ + resource_rva + (resource_index ? 6 : 3) * 12;
                    if (!read(slot, slot_bytes.data(), slot_bytes.size(), observation))
                    {
                        continue;
                    }
                    const auto handle = field<uint32_t>(slot_bytes, 4);
                    if (!resolve_request(handle, wrapper, observation) || wrapper.bytes[6] != 2 ||
                        field<uint32_t>(wrapper.bytes, 12) != active_child || !resolve_request(active_child, child, observation) ||
                        child.bytes[6] != 3 || field<uint32_t>(child.bytes, 12) != handle ||
                        (site_index == 5 ? child.address != rdi : child.address != rsi))
                    {
                        continue;
                    }
                    found = true;
                    if (resource.handle != handle)
                    {
                        resource.token = ++serial_;
                        resource.pre_free = false;
                        resource.native_assignment = false;
                    }
                    resource.handle = handle;
                    resource.child = active_child;
                    resource.wrapper = wrapper.address;
                    resource.tag = field<uint32_t>(slot_bytes, 8);
                    observation.recorder = static_cast<uint8_t>(resource_index);
                    observation.token = resource.token;
                    observation.flags |= matched | native_generation_unavailable;
                    if (site_index == 36 && transaction != 0xFFFF)
                    {
                        resource.world_owner = owner;
                        resource.world_context = context;
                        resource.transaction = transaction;
                        resource.native_assignment = true;
                    }
                    if (!resource.native_assignment || resource.world_owner != owner || resource.world_context != context ||
                        resource.transaction != transaction)
                    {
                        observation.flags |= earlier_unobserved | native_transaction_assignment_unavailable;
                    }
                    else
                    {
                        observation.flags |= native_transaction_assignment_matched;
                    }
                    if (site_index == 5)
                    {
                        observation.values = {handle,
                                              active_child,
                                              transaction,
                                              static_cast<uint32_t>(rax),
                                              wrapper.address,
                                              child.address,
                                              context,
                                              owner,
                                              0,
                                              0,
                                              0,
                                              resource.tag};
                        uint64_t offset{};
                        uint64_t stride{};
                        std::array<uint8_t, 6> transaction_bytes{};
                        if (transaction != 0xFFFF && 0x263C3D8 <= image_size_ && read(base_ + 0x263C3C0, &offset, 8, observation) &&
                            read(base_ + 0x263C3D0, &stride, 8, observation) && offset <= 0x100000 && stride >= 6 && stride <= 0x1000)
                        {
                            const auto transaction_address = base_ + 0x263C3C0 + offset + stride * transaction;
                            if (read(transaction_address, transaction_bytes.data(), transaction_bytes.size(), observation))
                            {
                                observation.values[8] = field<uint32_t>(transaction_bytes, 0);
                                observation.values[9] = transaction_bytes[4];
                                observation.values[10] = transaction_bytes[5];
                            }
                        }
                    }
                    else
                    {
                        observation.values = {handle, active_child, transaction, context, owner};
                    }
                    break;
                }
                if (!found)
                {
                    observation.flags |= identity_miss;
                }
            }
            else
            {
                switch (site_index)
                {
                case 4:
                    observation.recorder = 1;
                    observation.values = {static_cast<uint32_t>(rcx), r9};
                    if (r9 != base_ + resource_rva + 6 * 12)
                    {
                        observation.flags |= identity_miss;
                    }
                    break;
                case 6:
                    registry(r8, 3, 2, 4);
                    observation.values[1] = static_cast<uint32_t>(rdx);
                    break;
                case 7:
                    registry(rax, 1, 2, 3);
                    break;
                case 8:
                    registry(rcx, 1, 2, 3);
                    break;
                case 9: {
                    uint8_t previous{};
                    observation.values = {rax, rcx & 0xFF};
                    if (read_offset(rax, 1, &previous, 1, observation))
                    {
                        observation.values[2] = previous;
                    }
                    break;
                }
                case 10:
                    observation.values[0] = 1;
                    break;
                case 11:
                    observation.values[0] = rcx & 0xFF;
                    break;
                case 12: {
                    uint8_t flag{};
                    observation.values = {rax, rbp, 0, rbx & 0xFF};
                    if (rbp >= user_end - 1 || rax != rbp + 1 || (rbx & 0xFF) != 0)
                    {
                        observation.flags |= identity_miss;
                    }
                    else if (read(rax, &flag, 1, observation))
                    {
                        observation.values[2] = flag;
                    }
                    break;
                }
                case 13:
                    observation.values = {rcx, rax, static_cast<uint32_t>(rdx), r8 & 0xFF};
                    if (rcx != base_ + 0x1FB75F0 || rax != base_ + 0x1C18F98)
                    {
                        observation.flags |= identity_miss;
                    }
                    break;
                case 14:
                    observation.values = {static_cast<uint32_t>(rcx), static_cast<uint32_t>(rdx), rsi};
                    if (static_cast<uint32_t>(rcx) != 1 || rsi != base_ + 0x1FB75F0)
                    {
                        observation.flags |= identity_miss;
                    }
                    break;
                case 15:
                    observation.values = {rcx, rax};
                    if (rcx != base_ + 0x1FB75F0 || rax != base_ + 0x1C18F98)
                    {
                        observation.flags |= identity_miss;
                    }
                    break;
                case 16:
                case 17:
                    observation.values[0] = rcx;
                    if (rcx != base_ + 0x1FB75F0)
                    {
                        observation.flags |= identity_miss;
                    }
                    break;
                case 18:
                    capture_primary(rax);
                    break;
                case 19:
                case 21:
                    capture_primary(rcx);
                    break;
                case 20:
                    observation.values[0] = rax;
                    if (read(rax, observation.bytes.data(), 57, observation))
                    {
                        observation.length = 57;
                        observation.values[1] = field<uint64_t>(observation.bytes, 0);
                        observation.values[2] = observation.bytes[0x38];
                        observation.values[3] = rbx;
                    }
                    break;
                case 22:
                    observation.values = {rcx, rdx, 0, 0, rsp};
                    if (!sender || rdx != rbx || rcx != rdi || !read(rdx, observation.bytes.data(), 48, observation) ||
                        observation.bytes[0] != 1)
                    {
                        observation.flags |= identity_miss;
                    }
                    else
                    {
                        *sender = sender_state{.tid = observation.tid,
                                               .stage = 1,
                                               .token = ++serial_,
                                               .manager = rcx,
                                               .record_address = rdx,
                                               .identity = field<uint64_t>(observation.bytes, 1),
                                               .pump_rsp = rsp};
                        std::copy_n(observation.bytes.begin(), 48, sender->bytes.begin());
                        observation.values[2] = 1;
                        observation.values[3] = sender->identity;
                        observation.token = sender->token;
                        observation.length = 48;
                        observation.flags |= sender_matched;
                    }
                    break;
                case 23:
                    observation.values = {rbx, rax & 0xFF};
                    if (sender_frame() && sender->stage == 1)
                    {
                        observation.flags |= sender_matched;
                        sender->stage = (rax & 0xFF) ? 2 : 0;
                    }
                    else
                    {
                        observation.flags |= identity_miss;
                        if (sender)
                        {
                            sender->stage = 0;
                        }
                    }
                    break;
                case 24:
                    observation.values = {rcx, r8};
                    if (!sender_frame() || sender->stage != 2)
                    {
                        observation.flags |= identity_miss;
                    }
                    else
                    {
                        observation.flags |= sender_matched;
                    }
                    break;
                case 25:
                    observation.values = {rbx, rax};
                    if (sender_frame() && sender->stage == 2)
                    {
                        sender->stage = rax ? 3 : 0;
                        observation.flags |= sender_matched;
                    }
                    else
                    {
                        observation.flags |= identity_miss;
                        if (sender)
                        {
                            sender->stage = 0;
                        }
                    }
                    break;
                case 26:
                    observation.values[0] = rcx;
                    if (read(rcx, observation.bytes.data(), 48, observation))
                    {
                        observation.length = 48;
                    }
                    if (sender_frame() && sender->stage == 3 && rcx == rsp + 0x50 && !(observation.flags & read_failed))
                    {
                        sender->stage = 4;
                        observation.values[1] = sender->record_address;
                        observation.values[2] = sender->identity;
                        observation.flags |= sender_matched;
                    }
                    else
                    {
                        observation.flags |= identity_miss;
                        if (sender)
                        {
                            sender->stage = 0;
                        }
                    }
                    break;
                case 27: {
                    uint64_t arg5{};
                    uint64_t return_address{};
                    const auto argument_read = read_offset(rsp, 0x20, &arg5, 8, observation);
                    observation.values = {static_cast<uint32_t>(rcx), static_cast<uint32_t>(rdx), r8, r9, arg5, rsp};
                    if (static_cast<uint32_t>(rcx) != 503 || static_cast<uint32_t>(rdx) != 1 || r8 != rsp + 0x38 || !argument_read ||
                        arg5 != 0)
                    {
                        observation.flags |= identity_miss;
                        if (sender)
                        {
                            sender->stage = 0;
                        }
                    }
                    else if (dispatcher_frame_verified_ && sender && sender->stage == 4 && rsp <= sender->pump_rsp &&
                             sender->pump_rsp - rsp == 0x120 && read_offset(rsp, 0x78, &return_address, 8, observation) &&
                             return_address == base_ + 0xBE7E90)
                    {
                        sender->stage = 5;
                        sender->dispatch_rsp = rsp;
                        sender->descriptor = r8;
                        sender->completion_token = r9;
                        sender->dispatch_mode = static_cast<uint32_t>(rdx);
                        observation.values[6] = sender->record_address;
                        observation.values[7] = sender->identity;
                        observation.flags |= sender_matched;
                    }
                    else
                    {
                        observation.flags |= earlier_unobserved;
                        if (sender)
                        {
                            sender->stage = 0;
                        }
                    }
                    break;
                }
                case 28:
                    observation.values = {static_cast<uint32_t>(rcx), peer_frame()};
                    if (observation.values[1] && static_cast<uint32_t>(rcx) == 0)
                    {
                        observation.flags |= sender_matched;
                    }
                    else
                    {
                        observation.flags |= earlier_unobserved;
                    }
                    break;
                case 29:
                    observation.values = {
                        static_cast<uint32_t>(reg(x86_register::r12)), rdi & 0xFFFF, static_cast<uint32_t>(r14), r15, rbx, peer_frame()};
                    if ((rdi & 0xFFFF) != 503)
                    {
                        observation.flags |= identity_miss;
                    }
                    else if (observation.values[5])
                    {
                        observation.values[6] = sender->record_address;
                        observation.values[7] = sender->identity;
                        observation.flags |= sender_matched;
                    }
                    else
                    {
                        observation.flags |= earlier_unobserved;
                    }
                    if (sender)
                    {
                        sender->stage = 0;
                    }
                    break;
                case 30:
                    if (state.hits == 1)
                    {
                        baseline_locked(2, observation.host_ms);
                    }
                    observation.values[0] = 1;
                    break;
                case 31:
                case 32:
                    observation.values[0] = static_cast<uint32_t>(rax);
                    break;
                case 34:
                    registry(rbx, 1, 2, 3);
                    break;
                case 35: {
                    uint64_t object{};
                    uint64_t current{};
                    read(base_ + 0x2742FB0, &object, 8, observation);
                    registry(rbx, 1, 2, 5);
                    read(base_ + 0x2742FA0, &current, 8, observation);
                    observation.values[3] = current;
                    observation.values[4] = rax;
                    observation.values[6] = object;
                    if (rax != 0)
                    {
                        observation.flags |= identity_miss;
                    }
                    for (auto& flow : senders_)
                    {
                        flow.stage = 0;
                    }
                    break;
                }
                case 38: {
                    uint64_t return_address{};
                    const auto state_read = read_offset(rbx, 0x390, observation.bytes.data(), 24, observation);
                    const auto frame_read = read_offset(rsp, 0x328, &return_address, 8, observation);
                    const auto previous = static_cast<uint32_t>(r15);
                    const auto current = field<uint32_t>(observation.bytes, 0);
                    const auto current_reason = field<uint32_t>(observation.bytes, 4);
                    const auto timestamp = field<uint64_t>(observation.bytes, 8);
                    const auto goal = field<uint32_t>(observation.bytes, 16);
                    const auto goal_reason = field<uint32_t>(observation.bytes, 20);
                    observation.length = state_read ? 24 : 0;
                    observation.values = {rbx,
                                          previous,
                                          current,
                                          current_reason,
                                          timestamp,
                                          goal,
                                          goal_reason,
                                          rax,
                                          return_address,
                                          static_cast<uint32_t>(reg(x86_register::eflags)),
                                          cleanup_guards_verified_,
                                          budget_.cleanup_seen};
                    if (!cleanup_guards_verified_ || !state_read || !frame_read || return_address != base_ + 0xE23568 || current != 28 ||
                        goal != 28 || previous == 28 || current_reason != goal_reason || timestamp != rax || !budget_.armed)
                    {
                        observation.kind = unavailable;
                        observation.flags |= identity_miss;
                        break;
                    }
                    observation.flags |= matched;
                    if (previous == 23)
                    {
                        observation.flags |= investment_cleanup_transition_matched;
                        if (!budget_.cleanup_seen)
                        {
                            budget_.observe_cleanup(observation.host_ms);
                            record timer = observation;
                            timer.kind = cleanup_timer;
                            timer.recorder = 3;
                            timer.values = {budget_.armed_at,
                                            budget_.cleanup_at,
                                            destiny_startup_capture_budget::post_cleanup_ms,
                                            destiny_startup_capture_budget::total_ms,
                                            rbx,
                                            previous,
                                            current,
                                            current_reason,
                                            timestamp,
                                            goal,
                                            goal_reason,
                                            return_address};
                            enqueue_locked(timer, true);
                        }
                    }
                    else
                    {
                        observation.flags |= earlier_unobserved;
                    }
                    break;
                }
                default:
                    break;
                }
            }
            if (sender && observation.flags & sender_matched)
            {
                observation.token = sender->token;
            }
        }
        catch (...)
        {
            observation.flags |= read_failed;
        }
        if (observation.flags & identity_miss)
        {
            ++state.identity_misses;
        }
        if (observation.flags & read_failed)
        {
            ++state.read_failures;
        }
        const auto changed = !state.last_valid || state.last.recorder != observation.recorder || state.last.token != observation.token ||
                             state.last.flags != observation.flags || state.last.values != observation.values ||
                             state.last.length != observation.length || state.last.bytes != observation.bytes;
        if (changed)
        {
            state.last = observation;
            state.last_valid = true;
            if (enqueue_locked(observation))
            {
                ++state.emitted;
            }
        }
        const auto elapsed =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
        state.callback_ns += elapsed;
        state.callback_max_ns = std::max(state.callback_max_ns, elapsed);
        const bool valid = !(observation.flags & (read_failed | identity_miss));
        if (valid && ((site_index == 12 && observation.values[2] != 0) || (site_index == 20 && observation.values[2] != 0) ||
                      (site_index == 38 && (observation.flags & investment_cleanup_transition_matched))))
        {
            retire_site_locked(site_index, positive_gate, cpu.index());
        }
        else if (state.hits >= 128)
        {
            retire_site_locked(site_index, hit_budget, cpu.index());
        }
        uint64_t callback_total{};
        for (const auto& site : sites_)
        {
            callback_total += site.callback_ns;
        }
        if (!retired_ && callback_total >= 2000000000)
        {
            retire_locked(callback_budget, observation.host_ms);
        }
        if (retired_)
        {
            remove_hooks_locked(cpu.index());
        }
    }

    void destiny_startup_capture::retire_site_locked(const size_t index, const uint8_t reason, const size_t executing_vcpu)
    {
        auto& state = sites_[index];
        state.retirement = reason;
        if (index >= 22 && index <= 27)
        {
            for (auto& sender : senders_)
            {
                sender.stage = 0;
            }
        }
        remove_hooks_locked(executing_vcpu);
        record receipt{};
        receipt.kind = coverage;
        receipt.site = static_cast<uint8_t>(index);
        receipt.recorder = 3;
        receipt.host_ms = now_ms();
        receipt.values = {state.hits,
                          state.identity_misses,
                          state.read_failures,
                          state.signature_misses,
                          state.dropped,
                          state.emitted,
                          state.callback_ns,
                          state.callback_max_ns,
                          reason,
                          state.attempted,
                          static_cast<uint64_t>(state.hook != nullptr),
                          state.raw_traps.load(std::memory_order_relaxed)};
        enqueue_locked(receipt, true);
    }

    void destiny_startup_capture::retire_locked(const uint8_t reason, const uint64_t now)
    {
        if (retired_)
        {
            return;
        }
        retired_ = reason;
        if (focused_)
        {
            reason175_summary_locked(now);
        }
        record receipt{};
        receipt.kind = retirement;
        receipt.recorder = 3;
        receipt.site = 255;
        receipt.host_ms = now;
        receipt.values = {reason,
                          focused_ ? reason175_.armed_at : budget_.armed_at,
                          focused_ ? reason175_.first_at : budget_.cleanup_at,
                          focused_ ? reason175_.armed : budget_.armed,
                          focused_ ? reason175_.first_seen : budget_.cleanup_seen,
                          accepted_,
                          dropped_,
                          snapshots_,
                          snapshot_misses_,
                          host_remove_attempts_,
                          output_records_,
                          output_bytes_};
        enqueue_locked(receipt, true);
        for (size_t index = 0; index < selected_site_count(); ++index)
        {
            const auto& state = sites_[index];
            record summary{};
            summary.kind = coverage;
            summary.site = static_cast<uint8_t>(index);
            summary.recorder = 3;
            summary.host_ms = now;
            summary.values = {state.hits,
                              state.identity_misses,
                              state.read_failures,
                              state.signature_misses,
                              state.dropped,
                              state.emitted,
                              state.callback_ns,
                              state.callback_max_ns,
                              state.retirement ? state.retirement : reason,
                              state.attempted,
                              static_cast<uint64_t>(state.hook != nullptr),
                              state.raw_traps.load(std::memory_order_relaxed)};
            enqueue_locked(summary, true);
        }
        wake_.notify_all();
    }

    bool destiny_startup_capture::quiescent_locked(const size_t executing_vcpu) const
    {
        if (focused_ && owner_.vcpu_count() <= 64)
        {
            std::array<bool, 64> running{};
            for (size_t index = 0; index < owner_.vcpu_count(); ++index)
            {
                running[index] = owner_.vcpu(index).running.load(std::memory_order_relaxed);
            }
            return destiny_reason175_capture_state::quiescent(running.data(), owner_.vcpu_count(), executing_vcpu);
        }
        for (size_t index = 0; index < owner_.vcpu_count(); ++index)
        {
            if (index != executing_vcpu && owner_.vcpu(index).running.load(std::memory_order_relaxed))
            {
                return false;
            }
        }
        return true;
    }

    void destiny_startup_capture::remove_hooks_locked(const size_t executing_vcpu)
    {
        if (!quiescent_locked(executing_vcpu))
        {
            if (++quiescence_deferrals_ == 1)
            {
                record receipt{};
                receipt.kind = unavailable;
                receipt.recorder = 3;
                receipt.site = 253;
                receipt.host_ms = now_ms();
                receipt.values = {retired_, owner_.vcpu_count(), executing_vcpu, quiescence_deferrals_};
                enqueue_locked(receipt, true);
            }
            return;
        }
        bool remaining{};
        for (auto& site : sites_)
        {
            if (site.hook && (retired_ || site.retirement))
            {
                try
                {
                    owner_.emu().delete_hook(site.hook);
                    site.hook = nullptr;
                }
                catch (...)
                {
                    remaining = true;
                }
            }
        }
        if (retired_ && !remaining && !physical_retirement_)
        {
            physical_retirement_ = true;
            record receipt{};
            receipt.kind = removed;
            receipt.recorder = 3;
            receipt.site = 255;
            receipt.host_ms = now_ms();
            receipt.values = {retired_, host_remove_attempts_, 0, quiescence_deferrals_};
            enqueue_locked(receipt, true);
        }
    }

    void destiny_startup_capture::observe_unload(mapped_module& module)
    {
        if (module.image_base != base_)
        {
            return;
        }
        std::unique_lock kernel_guard(kernel_, std::defer_lock);
        if (!kernel_.is_held_by_current_thread())
        {
            kernel_guard.lock();
        }
        const std::scoped_lock state_guard(mutex_);
        retire_locked(module_unload, now_ms());
        remove_hooks_locked();
    }

    void destiny_startup_capture::maintenance(const std::stop_token& stop)
    {
        while (!stop.stop_requested())
        {
            bool remove{};
            {
                const std::scoped_lock state_guard(mutex_);
                const auto expired = capture_expiry(now_ms());
                if (!expired.empty())
                {
                    const auto closure_reason = focused_ ? reason175_closure : cleanup_budget;
                    const auto reason = expired == "total_budget" ? total_budget : closure_reason;
                    retire_locked(reason, now_ms());
                }
                remove = retired_ && !physical_retirement_;
                for (const auto& site : sites_)
                {
                    remove = remove || (site.retirement && site.hook);
                }
            }
            if (remove)
            {
                std::unique_lock kernel_guard(kernel_, std::try_to_lock);
                const std::scoped_lock state_guard(mutex_);
                ++host_remove_attempts_;
                if (kernel_guard.owns_lock())
                {
                    remove_hooks_locked();
                }
            }
            std::unique_lock state_guard(mutex_);
            if (retired_ && physical_retirement_)
            {
                return;
            }
            wake_.wait_for(state_guard, stop, std::chrono::milliseconds(50), [] { return false; });
        }
    }

    void destiny_startup_capture::output_worker(const std::stop_token& stop)
    {
        while (!stop.stop_requested())
        {
            drain();
            std::unique_lock state_guard(mutex_);
            if (retired_ && physical_retirement_ && final_receipt_ && !pending_count_)
            {
                return;
            }
            wake_.wait(state_guard, stop, [this] { return pending_count_ || (retired_ && physical_retirement_ && !final_receipt_); });
        }
        drain();
    }

    void destiny_startup_capture::drain()
    {
        std::array<record, 16> batch{};
        for (;;)
        {
            size_t count{};
            {
                const std::scoped_lock state_guard(mutex_);
                count = std::min(batch.size(), pending_count_);
                for (size_t index = 0; index < count; ++index)
                {
                    batch[index] = records_[(pending_begin_ + index) % records_.size()];
                }
                pending_begin_ = (pending_begin_ + count) % records_.size();
                pending_count_ -= count;
            }
            if (!count)
            {
                const std::scoped_lock state_guard(mutex_);
                if (!retired_ || !physical_retirement_ || final_receipt_)
                {
                    return;
                }
                final_receipt_ = true;
                record receipt{};
                receipt.kind = removed;
                receipt.recorder = 3;
                receipt.site = 255;
                receipt.host_ms = now_ms();
                receipt.values = {retired_,         host_remove_attempts_, accepted_ + 1, dropped_,       snapshots_,
                                  snapshot_misses_, output_records_,       output_bytes_, read_requests_, failed_read_requests_};
                for (const auto& site : sites_)
                {
                    receipt.values[10] += site.raw_traps.load(std::memory_order_relaxed);
                }
                receipt.values[11] = retired_traps_;
                if (focused_)
                {
                    receipt.flags = reason175_.complete(dropped_, failed_read_requests_)
                                        ? matched | reason175_producer_verified | reason175_setter_verified | reason175_commit_verified
                                        : earlier_unobserved | reason175_ambiguous;
                }
                enqueue_locked(receipt, true);
                continue;
            }
            for (size_t index = 0; index < count; ++index)
            {
                const auto& observation = batch[index];
                constexpr std::array<const char*, 4> focused_names{"blocked_reason_post_store", "native_cleanup_request_entry",
                                                                   "native_cleanup_setter_entry", "native_cleanup_commit"};
                constexpr std::array<const char*, 4> focused_fields{
                    "reason_pointer,reason,native_manager,latch,al,rsp,original_owner,outer_return,inner_return,middle_return,stage,read_"
                    "mask",
                    "target,reason,rsp,caller,context,config,status,elapsed_i64_bits,limit_i32_bits,read_mask,owner,rbp",
                    "manager,current,current_reason,timestamp,goal,goal_reason,target,reason,rsp,caller,wrapper_caller,read_mask",
                    "manager,previous,current,current_reason,timestamp,goal,goal_reason,rax,caller,eflags,guards_verified,read_mask"};
                const auto* site_name = "control";
                const auto* fields = "control";
                if (focused_)
                {
                    site_name = observation.site < focused_names.size() ? focused_names[observation.site] : "control";
                    fields = observation.site < focused_fields.size() ? focused_fields[observation.site] : "control";
                }
                else
                {
                    site_name = observation.site < sites.size() ? sites[observation.site].name : "control";
                    fields = observation.site < sites.size() ? sites[observation.site].fields : "control";
                }
                if (focused_ && observation.kind == signature && observation.site == 250)
                {
                    site_name = "native_frame_role_dependency_guard";
                    fields = "rva,full_length,dependency_index,readable,all_bytes_verified";
                }
                if (focused_ && observation.kind == signature && observation.site < focused_names.size())
                {
                    fields = "rva,full_length,site_index,readable,all_bytes_verified";
                }
                if (focused_ && observation.kind == unavailable && observation.site == 249)
                {
                    site_name = "four_hooks_installed";
                    fields = "site_count,dependency_span_count,all_dependencies_verified,arming_started_ms";
                }
                if (focused_ && observation.recorder == 5 && observation.site == 1)
                {
                    site_name = "blocked_request_consumer_frame";
                    fields =
                        "consumed_reason,reason_pointer,outer_return,owner,active_stage_mask,read_mask,rsp,rbp,producer_reason_pointer,"
                        "producer_owner,producer_outer_return,producer_tid";
                }
                if (focused_ && observation.kind == unavailable && observation.site == 251)
                {
                    site_name = "first_cleanup_chain";
                    fields = "first_any_reason,source,request_rsp,caller,manager,tid,first_sequence,first_seen,setter_seen,commit_seen,"
                             "ambiguous,producer_verified";
                }
                else if (observation.kind == baseline)
                {
                    site_name = observation.recorder < 2 ? "safe_resource_snapshot" : "safe_registry_snapshot";
                    fields = observation.recorder < 2 ? "phase,slot,state,detail,live_handle,tag,outstanding"
                                                      : "phase,active,registry,vtable,mode,content";
                }
                else if ((observation.kind == signature || observation.kind == unavailable) && observation.site == 252)
                {
                    site_name =
                        observation.kind == signature ? "controller_cleanup_dependency_guard" : "controller_cleanup_contract_availability";
                    fields = observation.kind == signature
                                 ? "rva,length,record_byte_offset,verified,readable"
                                 : "guards_verified,required_guard_spans,site_rva,return_rva,return_offset,post_cleanup_ms,total_ms";
                }
                else if (observation.kind == unavailable && observation.site == 254)
                {
                    site_name = observation.values[1] == 0xF2C280 ? "native_sender_frame_guards" : "request_pool_layout_guard";
                    fields = observation.values[1] == 0xF2C280
                                 ? "combined_verified,dispatcher_rva,return_offset,frame_size,three_guards_required"
                                 : "verified,rva,length,record_byte_offset";
                }
                else if (observation.kind == unavailable && observation.site == 253)
                {
                    site_name = "physical_removal_deferred";
                    fields = "global_reason,vcpu_count,exact_callback_vcpu_or_host_sentinel,quiescence_deferrals";
                }
                else if (observation.kind == activation)
                {
                    site_name = "native_investment_entry";
                    fields =
                        focused_
                            ? "image_base,image_size,total_ms,first_any_cleanup_closure_ms,max_records,max_bytes,site_hit_limit,callback_"
                              "wall_ns_limit"
                            : "image_base,image_size,total_ms,post_cleanup_ms,max_records,max_bytes,site_hit_limit,callback_wall_ns_limit";
                }
                else if (observation.kind == cleanup_timer)
                {
                    site_name = "controller_cleanup_timer_started_before_enter";
                    fields =
                        "armed_ms,cleanup_ms,post_cleanup_ms,total_ms,manager,previous_state,current_state,current_reason,native_timestamp,"
                        "goal_state,goal_reason,return_address";
                }
                else if (observation.kind == coverage)
                {
                    fields = "admitted_hits,identity_misses,callbacks_with_read_failure,signature_misses,drops,changed_records,callback_"
                             "wall_ns,max_"
                             "callback_wall_"
                             "ns,reason,attempted,hook_pending,raw_traps";
                }
                else if (observation.kind == retirement)
                {
                    fields = focused_ ? "reason,armed_ms,first_any_cleanup_ms,armed,first_any_cleanup_seen,accepted,drops,snapshots,"
                                        "snapshot_misses,host_remove_attempts,output_records,output_bytes"
                                      : "reason,armed_ms,cleanup_ms,armed,cleanup_seen,accepted,drops,snapshots,snapshot_misses,host_"
                                        "remove_attempts,output_records,output_bytes";
                }
                else if (observation.kind == removed)
                {
                    site_name = observation.values[2] ? "final_coverage" : "physical_hooks_removed";
                    fields = observation.values[2] ? "reason,host_remove_attempts,accepted,drops,snapshots,snapshot_misses,output_records_"
                                                     "before_receipt,charged_output_bytes_before_receipt,read_requests,failed_read_"
                                                     "requests,raw_traps,retired_traps"
                                                   : "reason,host_remove_attempts,reserved,quiescence_deferrals";
                }
                std::array<char, 2048> line{};
                const auto& values = observation.values;
                const auto length =
                    std::snprintf(line.data(), line.size(),
                                  "%s kind=%s recorder=%u site=%s host_ms=%llu tid=%u vcpu=%u rip=%#llx observation_token=%llu flags=%#llx "
                                  "fields=%s data=[%#llx,%#llx,%#llx,%#llx,%#llx,%#llx,%#llx,%#llx,%#llx,%#llx,%#llx,%#llx] bytes=",
                                  focused_ ? "FIRSTREASON175" : "DESTINYSTARTUP", kind_names[observation.kind], observation.recorder,
                                  site_name, static_cast<unsigned long long>(observation.host_ms), observation.tid, observation.vcpu,
                                  static_cast<unsigned long long>(observation.rip), static_cast<unsigned long long>(observation.token),
                                  static_cast<unsigned long long>(observation.flags), fields, static_cast<unsigned long long>(values[0]),
                                  static_cast<unsigned long long>(values[1]), static_cast<unsigned long long>(values[2]),
                                  static_cast<unsigned long long>(values[3]), static_cast<unsigned long long>(values[4]),
                                  static_cast<unsigned long long>(values[5]), static_cast<unsigned long long>(values[6]),
                                  static_cast<unsigned long long>(values[7]), static_cast<unsigned long long>(values[8]),
                                  static_cast<unsigned long long>(values[9]), static_cast<unsigned long long>(values[10]),
                                  static_cast<unsigned long long>(values[11]));
                size_t used = length > 0 ? std::min(static_cast<size_t>(length), line.size() - 1) : 0;
                for (size_t byte = 0; byte < observation.length && used + 3 < line.size(); ++byte)
                {
                    used += static_cast<size_t>(std::snprintf(line.data() + used, line.size() - used, "%02x", observation.bytes[byte]));
                }
                if (observation.kind == unavailable && observation.site == 255)
                {
                    used += static_cast<size_t>(std::snprintf(line.data() + used, line.size() - used,
                                                              " limitation=conditional_native_io_not_armed:ring_async_owner_edge_"
                                                              "unverified;live_pool_counter_unverified;title_baseline_external_only;"
                                                              "controller_cleanup_clock_requires_matched_native_23_to_28_commit;"
                                                              "unobserved_or_unavailable_or_other_previous_state_falls_back_to_300s"));
                }
                if (observation.kind == activation && !focused_)
                {
                    used += static_cast<size_t>(std::snprintf(
                        line.data() + used, line.size() - used,
                        " recorder_ids=0_resource3,1_resource6,2_native_gates,3_coverage "
                        "snapshot_phases=0_first_bootflow_not_title,1_investment,2_deadline "
                        "flags=1_read_failure,2_identity_miss,4_native_identity_matched,8_pre_release,16_status3_state4_path,32_earlier_"
                        "unobserved,"
                        "64_live_pool_counter_unverified,128_sender_callflow_matched,256_native_tx_assignment_matched,512_native_tx_"
                        "assignment_unavailable,1024_native_investment_to_cleanup_matched "
                        "observation_token_is_observer_serial;callback_wall_ns_is_admitted_observation_including_lock_wait_excluding_hook_"
                        "removal;"
                        "charged_bytes_include_ANSI9;proxy_maintenance_is_independent;physical_removal_waits_quiescent_boundary;"
                        "logger_sink_may_block_output_worker_join;controller_cleanup_clock_requires_verified_native_23_to_28_commit_before_"
                        "enter;"
                        "resource_free_does_not_start_it;cleanup_commit_does_not_prove_enter_return_or_task_drain"));
                }
                if (focused_ && observation.kind == activation)
                {
                    used += static_cast<size_t>(std::snprintf(
                        line.data() + used, line.size() - used,
                        " recorder_ids=4_focused,5_consumer_detail;source=0_unobserved,1_deadline,2_blocked_chain,3_other_or_unclassified_"
                        "request;"
                        "flags=1_read_failed,2_identity_miss,4_chain_matched,32_earlier_unobserved,2048_ambiguous,4096_producer,8192_"
                        "setter,16384_commit;"
                        "read_mask_is_field_availability:missing_bit_means_unavailable_not_zero;setter_commit_state_bytes_only_on_success;"
                        "first_is_first_admitted_any_reason_in_armed_coverage;serial_is_observer_order_not_guest_execution_order;"
                        "memory_samples_are_later_nonatomic_corroboration;deadline_taken_branch_proves_decision;"
                        "bounds_are_admission_limits;physical_removal_waits_quiescent_boundary_without_forcing_peers;"
                        "retired_INT3_can_exit_and_single_step_until_removed;callback_wall_includes_lock_wait_excludes_hook_removal;"
                        "final_coverage_required_for_complete_chain;summary_flags_are_provisional_until_final_no_drop_receipt;"
                        "commit_does_not_prove_enter_return_or_task_drain;charged_bytes_include_ANSI9;logger_sink_may_block_join"));
                }
                if (observation.kind == retirement || observation.kind == removed || observation.kind == coverage)
                {
                    const auto reason = observation.kind == coverage ? values[8] : values[0];
                    if (reason < reason_names.size())
                    {
                        used +=
                            static_cast<size_t>(std::snprintf(line.data() + used, line.size() - used, " reason=%s", reason_names[reason]));
                    }
                }
                used = std::char_traits<char>::length(line.data());
                if (used + 2 < line.size())
                {
                    line[used++] = '\n';
                    line[used] = '\0';
                }
                const auto is_coverage =
                    observation.kind == coverage || observation.kind == retirement || observation.kind == removed ||
                    observation.kind == cleanup_timer ||
                    (observation.kind == unavailable && (observation.site == 253 || (focused_ && observation.site == 251)));
                const auto output_byte_limit =
                    byte_limit() -
                    (is_coverage ? 0 : coverage_reserve() * (line.size() + destiny_startup_capture_budget::logger_envelope_bytes));
                const auto charged_bytes = used + destiny_startup_capture_budget::logger_envelope_bytes;
                bool output{};
                {
                    const std::scoped_lock state_guard(mutex_);
                    if ((focused_ &&
                         destiny_reason175_capture_state::output_allowed(output_records_, output_bytes_, charged_bytes, is_coverage)) ||
                        (!focused_ && output_records_ < record_limit() && charged_bytes <= output_byte_limit &&
                         output_bytes_ <= output_byte_limit - charged_bytes))
                    {
                        ++output_records_;
                        output_bytes_ += charged_bytes;
                        output = true;
                    }
                    else
                    {
                        ++dropped_;
                        if (observation.site < sites_.size())
                        {
                            ++sites_[observation.site].dropped;
                        }
                        if (focused_)
                        {
                            reason175_.ambiguous = true;
                        }
                        retire_locked(byte_budget, now_ms());
                    }
                }
                if (output)
                {
                    try
                    {
                        owner_.log.info("%s", line.data());
                    }
                    catch (...)
                    {
                        const std::scoped_lock state_guard(mutex_);
                        ++dropped_;
                        if (observation.site < sites_.size())
                        {
                            ++sites_[observation.site].dropped;
                        }
                        if (focused_)
                        {
                            reason175_.ambiguous = true;
                        }
                        retire_locked(output_failure, now_ms());
                    }
                }
            }
        }
    }

    void destiny_startup_capture::shutdown()
    {
        {
            const std::scoped_lock state_guard(mutex_);
            if (shutdown_)
            {
                return;
            }
            shutdown_ = true;
            retire_locked(destruction, now_ms());
        }
        worker_.request_stop();
        wake_.notify_all();
        if (worker_.joinable())
        {
            worker_.join();
        }
        {
            std::unique_lock kernel_guard(kernel_, std::defer_lock);
            if (!kernel_.is_held_by_current_thread())
            {
                kernel_guard.lock();
            }
            const std::scoped_lock state_guard(mutex_);
            remove_hooks_locked();
        }
        output_worker_.request_stop();
        wake_.notify_all();
        if (output_worker_.joinable())
        {
            output_worker_.join();
        }
    }
}
