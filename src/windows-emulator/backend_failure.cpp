#include "windows_emulator.hpp"
#include <disassembler.hpp>
#include <format>
#include <limits>

namespace sogen
{
    void windows_emulator::start_cpu(vcpu_context& vcpu, const size_t count)
    {
        try
        {
            vcpu.cpu.start(count);
        }
        catch (const std::exception& error)
        {
            const std::scoped_lock lock(this->kernel_lock_);
            this->record_stop(stop_reason::backend_error, error.what());
            this->log.error("// STOP: Emulation failure | tid %u (0x%X) | vCPU %u\n// REASON: %s\n",
                            vcpu.active_thread ? vcpu.active_thread->id : 0, vcpu.active_thread ? vcpu.active_thread->id : 0,
                            vcpu.cpu.index(), error.what());
            try
            {
                auto& cpu = vcpu.cpu;
                const auto address = cpu.reg<uint64_t>(x86_register::rip);
                std::string location{};
                if (const auto* module = this->mod_manager.find_by_address(address))
                {
                    location = std::format("{}+0x{:X}", module->name, address - module->image_base);
                    this->log.force_print(color::cyan, "// MODULE: %s | base 0x%llX | RVA 0x%llX | path %s\n", module->name.c_str(),
                                          static_cast<unsigned long long>(module->image_base),
                                          static_cast<unsigned long long>(address - module->image_base),
                                          u16_to_u8(module->module_path.u16string()).c_str());
                }
                else
                {
                    const auto region = this->memory.get_region_info(address);
                    location = std::format("allocation 0x{:X}+0x{:X}", region.allocation_base, address - region.allocation_base);
                    this->log.force_print(color::cyan, "// MODULE: no mapped image | allocation 0x%llX | region kind %u | committed %u\n",
                                          static_cast<unsigned long long>(region.allocation_base), static_cast<unsigned>(region.kind),
                                          static_cast<unsigned>(region.is_committed));
                }
                std::array<uint8_t, 15> bytes{};
                size_t size{};
                while (size < bytes.size() && address <= std::numeric_limits<uint64_t>::max() - size &&
                       cpu.try_read_memory(address + size, &bytes[size], 1))
                {
                    ++size;
                }
                const disassembler decoder{};
                const auto instructions =
                    decoder.disassemble(cpu, cpu.reg<uint16_t>(x86_register::cs), std::span(bytes).first(size), 1, address);
                std::string encoding{};
                const auto instruction_size = instructions.empty() ? size : instructions[0].size;
                for (size_t i = 0; i < instruction_size; ++i)
                {
                    encoding += std::format("{}{:02X}", i == 0 ? "" : " ", bytes[i]);
                }
                std::string assembly{};
                if (!instructions.empty())
                {
                    assembly = std::format("{} {}", instructions[0].mnemonic, instructions[0].op_str);
                }
                else
                {
                    assembly = size == 0 ? "instruction memory is unreadable" : "bytes could not be decoded";
                }
                this->log.force_print(color::yellow, "// ASM: 0x%llX (%s) | %s | %s\n", static_cast<unsigned long long>(address),
                                      location.c_str(), encoding.c_str(), assembly.c_str());
            }
            catch (const std::exception& diagnostic_error)
            {
                this->log.error("// DIAGNOSTIC: Could not inspect stopped instruction: %s\n", diagnostic_error.what());
            }
            throw;
        }
    }
}
