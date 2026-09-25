#pragma once
#include "x86_64_gdb_stub_handler.hpp"
#include "windows_filesystem.hpp"

#include <atomic>
#include <windows_emulator.hpp>
#include <memory_utils.hpp>
#include <utils/function.hpp>
#include <utils/finally.hpp>
#include <utils/string.hpp>

namespace sogen
{

    enum class gdb_target_architecture
    {
        automatic,
        bits_64,
        bits_32,
    };

    class win_x86_64_gdb_stub_handler : public x86_64_gdb_stub_handler
    {
      public:
        win_x86_64_gdb_stub_handler(windows_emulator& win_emu, utils::optional_function<bool()> should_stop = {},
                                    const gdb_target_architecture target_architecture = gdb_target_architecture::bits_64)
            : x86_64_gdb_stub_handler(win_emu.emu()),
              win_emu_(&win_emu),
              should_stop_(std::move(should_stop)),
              windows_filesystem_(win_emu),
              target_architecture_(target_architecture)
        {
            auto hook = [this](mapped_module&) {
                this->remember_stop_cpu(this->win_emu_->active_cpu());
                library_stop_pending_ = true;
                win_emu_->stop();
            };

            mod_load_id = win_emu_->callbacks.on_module_load.add(hook);
            mod_unload_id = win_emu_->callbacks.on_module_unload.add(hook);
            dbg_msg_id = win_emu_->callbacks.on_debug_string.add([this](std::string_view message) {
                this->remember_stop_cpu(this->win_emu_->active_cpu());
                debug_message.assign(message);
                action = gdb_stub::action::output;
                win_emu_->stop();
            });
        }

        ~win_x86_64_gdb_stub_handler() override
        {
            win_emu_->callbacks.on_module_load.remove(mod_load_id);
            win_emu_->callbacks.on_module_unload.remove(mod_unload_id);
            win_emu_->callbacks.on_debug_string.remove(dbg_msg_id);
        }

        void on_interrupt() override
        {
            this->interrupt_pending_ = true;
            this->win_emu_->stop();
        }

        void on_cpu_stop(cpu_interface& cpu) override
        {
            this->remember_stop_cpu(cpu);
            this->on_interrupt();
        }

        void on_idle() override
        {
            // UI event sinks acquire their own kernel lock; pumping must not retain it.
            this->win_emu_->ui().pump_events();
        }

        bool should_stop() override
        {
            return this->should_stop_();
        }

        bool execution_failed() const
        {
            return this->execution_failed_;
        }

        gdb_stub::action run() override
        {
            if (this->interrupt_pending_)
            {
                return this->stop_action();
            }
            this->execution_failed_ = false;
            try
            {
                if (!this->debuggee_paused())
                {
                    throw std::runtime_error("Cannot resume while a vCPU is running");
                }
                this->prepare_execution(false);
                this->stopped_vcpu_index_.store(UINT32_MAX, std::memory_order_release);
                this->clear_watchpoint_observations();
                this->run_in_progress_.store(true, std::memory_order_release);
                const auto finished = utils::finally([this] { this->run_in_progress_.store(false, std::memory_order_release); });
                this->win_emu_->start();
            }
            catch (const std::exception& e)
            {
                this->execution_failed_ = true;
                if (this->win_emu_->last_stop_reason() != stop_reason::backend_error)
                {
                    this->win_emu_->log.error("%s\n", e.what());
                }
            }

            return this->stop_action();
        }

        gdb_stub::action singlestep() override
        {
            if (this->interrupt_pending_)
            {
                return this->stop_action();
            }
            this->execution_failed_ = false;
            try
            {
                if (!this->debuggee_paused())
                {
                    throw std::runtime_error("Cannot step while a vCPU is running");
                }
                auto& vcpu = this->prepare_execution(true);

                vcpu.switch_thread = false;
                vcpu.thread().setup_if_necessary(vcpu.cpu, this->win_emu_->process);
                this->stopped_vcpu_index_.store(static_cast<uint32_t>(vcpu.cpu.index()), std::memory_order_release);
                this->clear_watchpoint_observations();
                this->run_in_progress_.store(true, std::memory_order_release);
                const auto finished = utils::finally([this] { this->run_in_progress_.store(false, std::memory_order_release); });
                this->win_emu_->start_cpu(vcpu, 1);
            }
            catch (const std::exception& e)
            {
                this->execution_failed_ = true;
                if (this->win_emu_->last_stop_reason() != stop_reason::backend_error)
                {
                    this->win_emu_->log.error("%s\n", e.what());
                }
            }

            return this->stop_action();
        }

        uint32_t get_current_thread_id() override
        {
            const auto* thread = this->stopped_vcpu().active_thread;
            return thread ? thread->id : 0;
        }

        std::vector<uint32_t> get_thread_ids() override
        {
            const auto& threads = this->win_emu_->process.threads;

            std::vector<uint32_t> ids{};
            ids.reserve(threads.size());

            for (const auto& t : threads | std::views::values)
            {
                if (!t.is_terminated())
                {
                    ids.push_back(t.id);
                }
            }

            return ids;
        }

        bool switch_to_thread(const uint32_t thread_id) override
        {
            if (thread_id == 0 || thread_id == UINT32_MAX)
            {
                this->selected_thread_ = std::nullopt;
                return true;
            }
            if (!this->find_live_thread(thread_id))
            {
                return false;
            }
            this->selected_thread_ = thread_id;
            return true;
        }

        bool select_general_thread(const uint32_t thread_id) override
        {
            return this->switch_to_thread(thread_id);
        }

        bool select_continuation_thread(const uint32_t thread_id) override
        {
            if (thread_id != 0 && thread_id != UINT32_MAX && !this->find_live_thread(thread_id))
            {
                return false;
            }
            this->continuation_thread_ = thread_id;
            return true;
        }

        size_t read_register(const size_t reg, void* data, const size_t max_length) override
        {
            return this->access_selected_registers(false,
                                                   [&](x86_64_cpu& cpu) { return this->read_cpu_register(cpu, reg, data, max_length); });
        }

        size_t write_register(const size_t reg, const void* data, const size_t size) override
        {
            return this->access_selected_registers(true, [&](x86_64_cpu& cpu) { return this->write_cpu_register(cpu, reg, data, size); });
        }

        bool read_memory(const uint64_t address, void* data, const size_t length) override
        {
            return this->debuggee_paused() && x86_64_gdb_stub_handler::read_memory(address, data, length);
        }

        bool write_memory(const uint64_t address, const void* data, const size_t length) override
        {
            return this->debuggee_paused() && x86_64_gdb_stub_handler::write_memory(address, data, length);
        }

        std::optional<uint32_t> get_exit_code() override
        {
            const auto status = this->win_emu_->process.exit_status;
            if (!status)
            {
                return std::nullopt;
            }

            return static_cast<uint32_t>(*status);
        }

        std::vector<gdb_stub::library_info> get_libraries() override
        {
            std::vector<gdb_stub::library_info> libs{};
            const auto& mod_manager = this->win_emu_->mod_manager;
            libs.reserve(this->win_emu_->mod_manager.modules().size());
            for (const auto& [base_addr, mod] : mod_manager.modules())
            {
                if (!mod.module_path.empty())
                {
                    libs.push_back({.name = mod.module_path.string(), .segment_address = base_addr + 0x1000});
                }
            }

            return libs;
        }

        std::string get_executable_path() override
        {
            const auto& mod_manager = this->win_emu_->mod_manager;
            return mod_manager.executable->module_path.string();
        }

        void reset_library_stop() override
        {
            library_stop_pending_ = false;
        }

        bool should_signal_library() override
        {
            return library_stop_pending_;
        }

        std::vector<gdb_stub::thread_info> get_thread_list() const override
        {
            std::vector<gdb_stub::thread_info> thread_list{};
            const auto& threads = this->win_emu_->process.threads;
            thread_list.reserve(threads.size());

            for (const auto& t : threads | std::views::values)
            {
                if (!t.is_terminated())
                {
                    thread_list.push_back({.id = t.id, .name = u16_to_u8(t.name)});
                }
            }

            return thread_list;
        }

        static const char* region_kind_name(const sogen::memory_region_kind kind)
        {
            switch (kind)
            {
            case sogen::memory_region_kind::free:
                return "free";
            case sogen::memory_region_kind::private_allocation:
                return "private";
            case sogen::memory_region_kind::file_section_view:
                return "file";
            case sogen::memory_region_kind::pagefile_section_view:
                return "pagefile";
            case sogen::memory_region_kind::section_image:
                return "image";
            case sogen::memory_region_kind::mmio:
                return "mmio";
            default:
                return "unknown";
            }
        }

        bool supports_memory_diagnostics() const override
        {
            return true;
        }

        std::vector<gdb_stub::memory_region_diagnostic> get_memory_regions() const override
        {
            std::vector<gdb_stub::memory_region_diagnostic> result;
            auto& memory = this->win_emu_->memory;
            for (const auto& [base, region] : memory.get_reserved_regions())
            {
                const auto* module_name = this->win_emu_->mod_manager.find_name(base);
                const std::string module =
                    (!module_name || std::string_view(module_name) == "<N/A>") ? std::string{} : std::string(module_name);
                const auto* kind = region_kind_name(region.kind);
                result.push_back({base, region.length, sogen::get_permission_string(region.initial_permission), "reserve", kind, module});
                for (const auto& [committed_base, committed] : region.committed_regions)
                {
                    auto protection = sogen::get_permission_string(committed.permissions.common);
                    if (committed.permissions.is_guarded())
                    {
                        protection += 'g';
                    }
                    result.push_back({committed_base, committed.length, protection, "commit", kind, module});
                }
            }
            return result;
        }

        bool supports_thread_diagnostics() const override
        {
            return true;
        }

        std::vector<gdb_stub::thread_diagnostic> get_thread_diagnostics() const override
        {
            std::vector<gdb_stub::thread_diagnostic> result;
            for (const auto& [index, thread] : this->win_emu_->process.threads)
            {
                gdb_stub::thread_diagnostic row{.id = thread.id};
                auto add = [&](const std::string& name, const auto value) { row.fields.emplace_back(name, std::to_string(value)); };
                auto address = [&](const std::string& name, const uint64_t value) {
                    row.fields.emplace_back(name, "0x" + utils::string::to_hex_number(value));
                    if (const auto* module = this->win_emu_->mod_manager.find_by_address(value))
                    {
                        row.fields.emplace_back(name + "_module", module->name);
                        row.fields.emplace_back(name + "_rva", "0x" + utils::string::to_hex_number(value - module->image_base));
                    }
                };
                row.fields.emplace_back("handle",
                                        "0x" + utils::string::to_hex_number(this->win_emu_->process.threads.make_handle(index).bits));
                row.fields.emplace_back("name", u16_to_u8(thread.name));
                vcpu_context* owner{};
                for (uint32_t i = 0; i < this->win_emu_->vcpu_count(); ++i)
                {
                    auto& vcpu = this->win_emu_->vcpu(i);
                    if (vcpu.active_thread == &thread)
                    {
                        owner = &vcpu;
                        break;
                    }
                }
                add("active", owner != nullptr);
                if (owner)
                {
                    add("vcpu", owner->cpu.index());
                }
                add("terminated", thread.is_terminated());
                add("initialized", thread.setup_done);
                add("suspended", thread.suspended);
                add("instructions", thread.executed_instructions);
                add("blocks", thread.executed_blocks);
                address("start", thread.start_address);
                address("last_instruction", thread.current_ip);
                if (owner)
                {
                    address("rip", owner->cpu.reg<uint64_t>(x86_register::rip));
                }
                if (thread.exit_status)
                {
                    row.fields.emplace_back("exit_status", "0x" + utils::string::to_hex_number(static_cast<uint32_t>(*thread.exit_status)));
                }
                if (thread.pending_status)
                {
                    row.fields.emplace_back("pending_status",
                                            "0x" + utils::string::to_hex_number(static_cast<uint32_t>(*thread.pending_status)));
                }
                std::string handles;
                std::string wait_threads;
                for (const auto handle : thread.await_objects)
                {
                    if (!handles.empty())
                    {
                        handles += ',';
                    }
                    handles += "0x" + utils::string::to_hex_number(handle.bits);
                    if (const auto* target = this->win_emu_->process.threads.get(handle))
                    {
                        if (!wait_threads.empty())
                        {
                            wait_threads += ',';
                        }
                        wait_threads += "0x" + utils::string::to_hex_number(target->id);
                    }
                }
                row.fields.emplace_back("wait_handles", handles);
                row.fields.emplace_back("wait_thread_ids", wait_threads);
                add("wait_any", thread.await_any);
                add("wait_alert", thread.waiting_for_alert);
                add("alerted", thread.alerted);
                add("apc_alertable", thread.apc_alertable);
                add("pending_apcs", thread.pending_apcs.size());
                add("wait_message", thread.await_msg.has_value());
                add("wait_message_mask_present", thread.await_msg_mask.has_value());
                add("wait_message_mask", thread.await_msg_mask.value_or(0));
                add("queued_messages", thread.message_queue.size());
                add("wait_io_completion", thread.await_io_completion.has_value());
                add("wait_host_condition", static_cast<bool>(thread.await_host_condition));
                add("callback_depth", thread.callback_stack.size());
                if (thread.await_time)
                {
                    if (*thread.await_time == std::chrono::steady_clock::time_point::min())
                    {
                        row.fields.emplace_back("wait_deadline", "infinite");
                    }
                    else
                    {
                        add("wait_deadline_steady_ns",
                            std::chrono::duration_cast<std::chrono::nanoseconds>(thread.await_time->time_since_epoch()).count());
                    }
                }
                result.push_back(std::move(row));
            }
            return result;
        }

        uint64_t get_thread_teb_addr(uint32_t id) const override
        {
            for (const auto& t : this->win_emu_->process.threads | std::views::values)
            {
                if (t.id == id)
                {
                    if (!t.is_terminated() && t.teb64)
                    {
                        return t.teb64->value();
                    }

                    break;
                }
            }

            return 0;
        }

        std::string consume_debug_output() override
        {
            action = gdb_stub::action::resume;
            return std::exchange(debug_message, std::string{});
        }

        std::string get_os_abi() override
        {
            return "Windows";
        }

        bool is_32_bit() const override
        {
            switch (target_architecture_)
            {
            case gdb_target_architecture::automatic:
                return win_emu_->process.is_wow64_process;
            case gdb_target_architecture::bits_32:
                return true;
            case gdb_target_architecture::bits_64:
                return false;
            }

            throw std::runtime_error("Invalid GDB target architecture");
        }

        gdb_stub::filesystem_interface* get_filesystem() override
        {
            return &windows_filesystem_;
        }

      private:
        emulator_thread* find_live_thread(const uint32_t id) const
        {
            for (auto& thread : this->win_emu_->process.threads | std::views::values)
            {
                if (thread.id == id && !thread.is_terminated())
                {
                    return &thread;
                }
            }
            return nullptr;
        }

        template <typename F>
        size_t access_selected_registers(const bool write, F&& access)
        {
            try
            {
                if (!this->debuggee_paused())
                {
                    return 0;
                }
                auto* thread = this->selected_thread_ ? this->find_live_thread(*this->selected_thread_) : this->stopped_vcpu().active_thread;
                if (!thread)
                {
                    return 0;
                }
                for (uint32_t i = 0; i < this->win_emu_->vcpu_count(); ++i)
                {
                    auto& vcpu = this->win_emu_->vcpu(i);
                    if (vcpu.active_thread == thread)
                    {
                        return access(vcpu.cpu);
                    }
                }
                if (thread->last_registers.empty())
                {
                    return 0;
                }
                auto& cpu = this->win_emu_->vcpu(0).cpu;
                const auto saved = cpu.save_registers();
                const auto restore = utils::finally([&] { cpu.restore_registers(saved); });
                // thread.restore() also changes the guest WOW64 GDT; inspection only loads backend registers.
                cpu.restore_registers(thread->last_registers);
                const auto result = access(cpu);
                if (write && result != 0)
                {
                    thread->last_registers = cpu.save_registers();
                }
                return result;
            }
            catch (...)
            {
                return 0;
            }
        }

        uint32_t get_watchpoint_thread_id(cpu_interface& cpu) override
        {
            const auto& vcpu = this->win_emu_->vcpu(static_cast<uint32_t>(cpu.index()));
            return vcpu.active_thread ? vcpu.active_thread->id : 0;
        }

        bool debuggee_paused() const
        {
            if (this->run_in_progress_.load(std::memory_order_acquire))
            {
                return false;
            }
            for (uint32_t i = 0; i < this->win_emu_->vcpu_count(); ++i)
            {
                if (this->win_emu_->vcpu(i).running.load(std::memory_order_acquire))
                {
                    return false;
                }
            }
            return true;
        }

        vcpu_context& stopped_vcpu() const
        {
            const auto index = this->stopped_vcpu_index_.load(std::memory_order_acquire);
            if (index < this->win_emu_->vcpu_count() && this->win_emu_->vcpu(index).active_thread)
            {
                return this->win_emu_->vcpu(index);
            }
            for (uint32_t i = 0; i < this->win_emu_->vcpu_count(); ++i)
            {
                if (this->win_emu_->vcpu(i).active_thread)
                {
                    return this->win_emu_->vcpu(i);
                }
            }
            return this->win_emu_->vcpu(0);
        }

        void remember_stop_cpu(cpu_interface& cpu)
        {
            auto expected = UINT32_MAX;
            this->stopped_vcpu_index_.compare_exchange_strong(expected, static_cast<uint32_t>(cpu.index()),
                                                             std::memory_order_acq_rel);
        }

        vcpu_context& prepare_execution(const bool step)
        {
            auto* vcpu = &this->stopped_vcpu();
            auto target = this->continuation_thread_;
            if (!target && step)
            {
                target = this->selected_thread_;
            }
            this->continuation_thread_ = std::nullopt;
            this->selected_thread_ = std::nullopt;
            if (target && *target != 0 && *target != UINT32_MAX)
            {
                auto* thread = this->find_live_thread(*target);
                if (!thread)
                {
                    throw std::runtime_error("Cannot find debugger continuation thread");
                }
                for (uint32_t i = 0; i < this->win_emu_->vcpu_count(); ++i)
                {
                    auto& candidate = this->win_emu_->vcpu(i);
                    if (candidate.active_thread == thread)
                    {
                        vcpu = &candidate;
                        break;
                    }
                }
                if (vcpu->active_thread != thread && !this->win_emu_->activate_thread(*vcpu, *target))
                {
                    throw std::runtime_error("Cannot activate debugger continuation thread");
                }
            }
            for (uint32_t i = 0; i < this->win_emu_->vcpu_count(); ++i)
            {
                this->win_emu_->vcpu(i).cpu.acknowledge_stop();
            }
            return *vcpu;
        }

        std::optional<uint32_t> selected_thread_{};
        std::optional<uint32_t> continuation_thread_{};

        gdb_stub::action stop_action()
        {
            this->selected_thread_ = std::nullopt;
            this->continuation_thread_ = std::nullopt;
            // Output packets auto-resume in the GDB stub; retain a concurrent interrupt until the following stop reply.
            if (action != gdb_stub::action::output)
            {
                this->interrupt_pending_ = false;
            }
            return action;
        }

        bool execution_failed_{};
        std::atomic_bool interrupt_pending_{false};
        std::atomic_bool run_in_progress_{false};
        std::atomic<uint32_t> stopped_vcpu_index_{UINT32_MAX};
        windows_emulator* win_emu_{};
        utils::optional_function<bool()> should_stop_{};
        windows_filesystem windows_filesystem_;
        gdb_target_architecture target_architecture_{gdb_target_architecture::bits_64};

        // Track library stop events
        std::atomic<bool> library_stop_pending_{true};
        std::string debug_message{};
        gdb_stub::action action{gdb_stub::action::resume};
        utils::callback_id_type mod_load_id{};
        utils::callback_id_type mod_unload_id{};
        utils::callback_id_type dbg_msg_id{};
    };

} // namespace sogen
