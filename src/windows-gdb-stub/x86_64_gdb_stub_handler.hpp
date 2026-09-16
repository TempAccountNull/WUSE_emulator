#pragma once
#include <gdb_stub.hpp>
#include <scoped_hook.hpp>
#include <arch_emulator.hpp>

#include <utils/concurrency.hpp>
#include <utils/finally.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

#include "x86_register_mapping.hpp"
#include "x86_target_descriptions.hpp"

#include "x64_register_mapping.hpp"
#include "x64_target_descriptions.hpp"

namespace sogen
{

    struct breakpoint_key
    {
        uint64_t addr{};
        size_t size{};
        gdb_stub::breakpoint_type type{};

        bool operator==(const breakpoint_key& other) const
        {
            return this->addr == other.addr && this->size == other.size && this->type == other.type;
        }
    };

} // namespace sogen

namespace std
{
    template <>
    struct hash<sogen::breakpoint_key>
    {
        std::size_t operator()(const sogen::breakpoint_key& k) const noexcept
        {
            return ((std::hash<uint64_t>()(k.addr) ^ (std::hash<size_t>()(k.size) << 1)) >> 1) ^
                   (std::hash<size_t>()(static_cast<size_t>(k.type)) << 1);
        }
    };
}

namespace sogen
{

    class x86_64_gdb_stub_handler : public gdb_stub::debugging_handler
    {
      public:
        x86_64_gdb_stub_handler(x86_64_emulator& emu)
            : emu_(&emu)
        {
        }

        ~x86_64_gdb_stub_handler() override = default;

        gdb_stub::action run() override
        {
            try
            {
                this->clear_watchpoint_observations();
                this->emu_->start();
            }
            catch (const std::exception& e)
            {
                puts(e.what());
            }

            return gdb_stub::action::resume;
        }

        gdb_stub::action singlestep() override
        {
            try
            {
                this->clear_watchpoint_observations();
                this->emu_->start(1);
            }
            catch (const std::exception& e)
            {
                puts(e.what());
            }

            return gdb_stub::action::resume;
        }

        size_t get_register_count() override
        {
            return this->get_register_mapping().size();
        }

        size_t get_max_register_size() override
        {
            return 512 / 8;
        }

        size_t read_register(const size_t reg, void* data, const size_t max_length) override
        {
            return this->read_cpu_register(*this->emu_, reg, data, max_length);
        }

        size_t read_cpu_register(x86_64_cpu& cpu, const size_t reg, void* data, const size_t max_length) const
        {
            try
            {
                const auto& registers = this->get_register_mapping();
                if (reg >= registers.size())
                {
                    return 0;
                }

                const auto real_reg = registers[reg];

                auto size = cpu.read_register(real_reg.reg, data, max_length);

                if (real_reg.offset)
                {
                    size -= *real_reg.offset;
                    memcpy(data, static_cast<uint8_t*>(data) + *real_reg.offset, size);
                }

                const auto result_size = real_reg.expected_size.value_or(size);

                if (result_size > size)
                {
                    memset(static_cast<uint8_t*>(data) + size, 0, result_size - size);
                }

                return result_size;
            }
            catch (...)
            {
                return 0;
            }
        }

        size_t write_register(const size_t reg, const void* data, const size_t size) override
        {
            return this->write_cpu_register(*this->emu_, reg, data, size);
        }

        size_t write_cpu_register(x86_64_cpu& cpu, const size_t reg, const void* data, const size_t size)
        {
            try
            {
                const auto& registers = this->get_register_mapping();
                if (reg >= registers.size())
                {
                    return 0;
                }

                const auto real_reg = registers[reg];

                size_t written_size = 0;

                if (real_reg.offset)
                {
                    std::vector<std::byte> full_data{};
                    full_data.resize(this->get_max_register_size());

                    written_size = cpu.read_register(real_reg.reg, full_data.data(), full_data.size());
                    if (written_size < *real_reg.offset)
                    {
                        return 0;
                    }

                    memcpy(full_data.data() + *real_reg.offset, data, written_size - *real_reg.offset);
                    cpu.write_register(real_reg.reg, full_data.data(), written_size);
                    written_size -= *real_reg.offset;
                }
                else
                {
                    written_size = cpu.write_register(real_reg.reg, data, size);
                }

                return real_reg.expected_size.value_or(written_size);
            }
            catch (...)
            {
                return 0;
            }
        }

        bool read_memory(const uint64_t address, void* data, const size_t length) override
        {
            return this->emu_->try_read_memory(address, data, length);
        }

        bool write_memory(const uint64_t address, const void* data, const size_t length) override
        {
            try
            {
                // GDB memory edits are debugger actions, not writes performed by the inferior.
                this->debugger_memory_write_depth_.fetch_add(1, std::memory_order_acq_rel);
                [[maybe_unused]] const auto restore =
                    utils::finally([this] { this->debugger_memory_write_depth_.fetch_sub(1, std::memory_order_acq_rel); });
                this->emu_->write_memory(address, data, length);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool set_breakpoint(const gdb_stub::breakpoint_type type, const uint64_t addr, const size_t size) override
        {
            try
            {
                if (size == 0 || size > std::numeric_limits<uint64_t>::max() - addr)
                {
                    return false;
                }
                return this->hooks_.access<bool>([&](hook_map& hooks) {
                    hooks[{addr, size, type}] = this->create_hook(type, addr, size);
                    return true;
                });
            }
            catch (...)
            {
                return false;
            }
        }

        bool delete_breakpoint(const gdb_stub::breakpoint_type type, const uint64_t addr, const size_t size) override
        {
            try
            {
                return this->hooks_.access<bool>([&](hook_map& hooks) {
                    const auto entry = hooks.find({addr, size, type});
                    if (entry == hooks.end())
                    {
                        return false;
                    }

                    hooks.erase(entry);

                    return true;
                });
            }
            catch (...)
            {
                return false;
            }
        }

        void on_interrupt() override
        {
            this->emu_->stop();
        }

        std::string get_target_description(const std::string_view file) override
        {
            const auto& target_descriptions = this->get_target_descriptions();
            const auto entry = target_descriptions.find(file);
            if (entry == target_descriptions.end())
            {
                return {};
            }

            auto data = entry->second;

            if (const auto os_abi = this->get_os_abi(); !os_abi.empty())
            {
                auto start_pos = data.find(osabi_template);
                if (start_pos != std::string::npos)
                {
                    data.replace(start_pos, osabi_template.length(), "<osabi>" + os_abi + "</osabi>");
                }
            }

            return data;
        }

        const std::vector<register_entry>& get_register_mapping() const
        {
            if (this->is_32_bit())
            {
                return x86_gdb_registers;
            }

            return x64_gdb_registers;
        }

        const std::map<std::string, std::string, std::less<>>& get_target_descriptions() const
        {
            if (this->is_32_bit())
            {
                return x86_target_descriptions;
            }

            return x64_target_descriptions;
        }

        uint32_t get_current_thread_id() override
        {
            return 1;
        }

        std::vector<uint32_t> get_thread_ids() override
        {
            return {this->get_current_thread_id()};
        }

        bool supports_watchpoint_diagnostics() const override
        {
            return true;
        }

        gdb_stub::watchpoint_stop get_watchpoint_observations() const override
        {
            return this->watchpoint_stop_.copy();
        }

        virtual bool is_32_bit() const = 0;

      protected:
        virtual uint32_t get_watchpoint_thread_id(cpu_interface& cpu)
        {
            (void)cpu;
            return this->get_current_thread_id();
        }

        void clear_watchpoint_observations()
        {
            this->watchpoint_stop_.access([](auto& stop) { stop = {}; });
        }

        void record_watchpoint(cpu_interface& cpu, uint64_t watched_address, size_t watched_size, uint64_t access_address, const void* data,
                               size_t access_size, bool write, memory_write_result result)
        {
            gdb_stub::watchpoint_observation observation{};
            observation.watched_address = watched_address;
            observation.watched_size = watched_size;
            observation.address = access_address;
            observation.size = access_size;
            observation.cpu_index = cpu.index();
            observation.thread_id = this->get_watchpoint_thread_id(cpu);
            observation.write = write;
            observation.host_write = result.origin == memory_write_origin::host;
            switch (result.outcome)
            {
            case memory_access_outcome::completed:
                observation.outcome = gdb_stub::watchpoint_outcome::completed;
                break;
            case memory_access_outcome::failed:
                observation.outcome = gdb_stub::watchpoint_outcome::failed;
                break;
            default:
                break;
            }
            observation.backend_error = result.backend_error;
            try
            {
                auto& x86_cpu = dynamic_cast<x86_64_cpu&>(cpu);
                observation.callback_pc = x86_cpu.read_instruction_pointer();
                observation.pc_valid = true;
            }
            catch (...)
            {
                // Preserve the access even if this backend cannot supply a PC at callback time.
            }
            observation.captured_address = access_address;
            if (data && observation.host_write)
            {
                const auto access_end = access_size > std::numeric_limits<uint64_t>::max() - access_address
                                            ? std::numeric_limits<uint64_t>::max()
                                            : access_address + access_size;
                const auto watched_end = watched_size > std::numeric_limits<uint64_t>::max() - watched_address
                                             ? std::numeric_limits<uint64_t>::max()
                                             : watched_address + watched_size;
                observation.captured_address = std::max(access_address, watched_address);
                const auto overlap_end = std::min(access_end, watched_end);
                if (observation.captured_address < overlap_end)
                {
                    const auto source_offset = observation.captured_address - access_address;
                    observation.captured_size = std::min<size_t>(overlap_end - observation.captured_address, observation.value.size());
                    std::memcpy(observation.value.data(), static_cast<const uint8_t*>(data) + source_offset, observation.captured_size);
                }
            }
            else if (data)
            {
                observation.captured_size = std::min(access_size, observation.value.size());
                std::memcpy(observation.value.data(), data, observation.captured_size);
            }
            this->watchpoint_stop_.access([&](auto& stop) {
                if (stop.count < stop.observations.size())
                {
                    stop.observations[stop.count++] = observation;
                }
                else
                {
                    ++stop.dropped;
                }
            });
        }

      private:
        x86_64_emulator* emu_{};

        utils::concurrency::container<gdb_stub::watchpoint_stop> watchpoint_stop_{};
        std::atomic_uint32_t debugger_memory_write_depth_{};
        using hook_map = std::unordered_map<breakpoint_key, scoped_hook>;
        utils::concurrency::container<hook_map> hooks_{};

        emulator_hook* create_execute_hook(const uint64_t addr, const size_t size)
        {
            return this->emu_->hook_memory_range_execution(addr, size, [this](cpu_interface&, const uint64_t) {
                this->on_interrupt(); //
            });
        }

        emulator_hook* create_read_hook(const uint64_t watched_address, const size_t watched_size)
        {
            return this->emu_->hook_memory_read(
                watched_address, watched_size,
                [this, watched_address, watched_size](cpu_interface& cpu, uint64_t access_address, const void* data, size_t access_size) {
                    this->record_watchpoint(cpu, watched_address, watched_size, access_address, data, access_size, false, {});
                    this->on_interrupt(); //
                });
        }

        emulator_hook* create_write_hook(const uint64_t watched_address, const size_t watched_size)
        {
            return this->emu_->hook_memory_write_observed(
                watched_address, watched_size,
                [this, watched_address, watched_size](cpu_interface& cpu, uint64_t access_address, const void* data, size_t access_size,
                                                      memory_write_result result) {
                    if (result.origin == memory_write_origin::host &&
                        this->debugger_memory_write_depth_.load(std::memory_order_acquire) != 0)
                    {
                        return;
                    }
                    this->record_watchpoint(cpu, watched_address, watched_size, access_address, data, access_size, true, result);
                    this->on_interrupt(); //
                });
        }

        scoped_hook create_hook(const gdb_stub::breakpoint_type type, const uint64_t addr, const size_t size)
        {
            using enum gdb_stub::breakpoint_type;

            switch (type)
            {
            case software:
            case hardware_exec:
                return {*this->emu_, this->create_execute_hook(addr, size)};
            case hardware_read:
                return {*this->emu_, this->create_read_hook(addr, size)};
            case hardware_write:
                return {*this->emu_, this->create_write_hook(addr, size)};
            case hardware_read_write:
                return {*this->emu_, {this->create_read_hook(addr, size), this->create_write_hook(addr, size)}};
            default:
                throw std::runtime_error("Bad bp type");
            }
        }
    };

} // namespace sogen
