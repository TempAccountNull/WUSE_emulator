#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace sogen
{

    enum class syscall_classification
    {
        regular,
        inline_syscall,
        crafted_out_of_line,
    };

    struct event_header
    {
        uint64_t sequence{};
        uint64_t instruction_count{};
    };

    struct execution_context
    {
        uint32_t thread_id{};
        uint64_t rip{};
        std::string rip_module{"<N/A>"};
        std::optional<uint64_t> previous_ip{};
        std::optional<std::string> previous_ip_module{};
    };

    struct observation_event
    {
        event_header header{};
        execution_context execution{};
    };

    struct summary_event
    {
        event_header header{};
    };

    struct run_started_event : summary_event
    {
        std::string backend_name{};
        std::string mode{"application"};
        std::string application{};
        std::vector<std::string> arguments{};
    };

    struct run_finished_event : summary_event
    {
        bool success{};
        std::optional<uint32_t> exit_status{};
        bool checkpoint_saved{};
        uint64_t rip{};
    };

    struct run_failed_event : summary_event
    {
        uint64_t rip{};
        std::string message{};
        std::string phase{"emulation"};
    };

    struct instruction_summary_entry
    {
        std::string mnemonic{};
        uint64_t count{};
    };

    struct instruction_summary_event : summary_event
    {
        std::vector<instruction_summary_entry> entries{};
    };

    struct buffered_stdout_event : summary_event
    {
        std::string data{};
    };

    struct stdout_chunk_event : observation_event
    {
        std::string data{};
    };

    struct suspicious_activity_event : observation_event
    {
        std::string details{};
        std::string decoded_instruction{};
    };

    struct debug_print_argument
    {
        std::string name{};
        uint64_t raw{};
        std::string text{};
        std::string encoding{};
        std::string bytes_hex{};
        std::string error{};
    };

    struct debug_print_call_event : observation_event
    {
        uint64_t call_id{};
        std::string api{};
        uint32_t pointer_bits{};
        uint64_t stack_pointer{};
        uint64_t return_address{};
        std::string return_module{};
        std::vector<debug_print_argument> arguments{};
        std::string error{};
    };

    struct debug_string_event : observation_event
    {
        std::string details{};
        std::string transport{};
        std::vector<uint64_t> origin_calls{};
        uint64_t data_address{};
        uint64_t byte_length{};
        std::string encoding{};
        std::string bytes_hex{};
        std::string error{};
        std::optional<debug_print_argument> ansi_fallback{};
        uint32_t component{};
        uint32_t level{};
    };

    struct generic_activity_event : observation_event
    {
        std::string details{};
    };

    struct generic_access_event : observation_event
    {
        std::string type{};
        std::string name{};
    };

    struct memory_allocate_event : observation_event
    {
        uint64_t address{};
        uint64_t length{};
        std::string permissions{};
        bool commit{};
    };

    struct memory_protect_event : observation_event
    {
        uint64_t address{};
        uint64_t length{};
        std::string permissions{};
    };

    struct fault_region_snapshot
    {
        uint64_t start{};
        uint64_t length{};
        uint64_t allocation_base{};
        uint64_t allocation_length{};
        std::string permissions{};
        std::string kind{};
        bool reserved{};
        bool committed{};
        bool guarded{};
    };

    struct fault_address_snapshot
    {
        uint64_t address{};
        std::optional<std::string> module{};
        std::optional<uint64_t> module_base{};
        std::optional<uint64_t> module_rva{};
        std::optional<fault_region_snapshot> region{};
        std::string error{};
    };

    struct fault_instruction_snapshot
    {
        fault_address_snapshot location{};
        std::string bytes_hex{};
        std::string assembly{};
        uint32_t decoded_size{};
        std::string error{};
    };

    struct fault_register_snapshot
    {
        std::string name{};
        std::optional<uint64_t> value{};
        std::string error{};
    };

    struct fault_stack_snapshot
    {
        std::optional<uint32_t> pointer_bits{};
        std::optional<uint32_t> address_bits{};
        std::optional<uint64_t> segment_base{};
        std::string width_source{};
        std::string address_source{};
        std::optional<uint64_t> address{};
        std::optional<uint64_t> value{};
        std::optional<fault_address_snapshot> value_location{};
        std::string error{};
    };

    struct memory_violation_event : observation_event
    {
        struct private_execute_memory_row
        {
            uint64_t address{};
            std::string bytes_hex{};
            uint32_t readable_bytes{};
        };

        uint64_t address{};
        uint64_t size{};
        std::string operation{};
        std::string violation_type{};
        fault_address_snapshot fault_address{};
        fault_instruction_snapshot actual_instruction{};
        std::optional<fault_instruction_snapshot> last_tracked_instruction{};
        std::vector<fault_register_snapshot> registers{};
        std::optional<uint32_t> code_bits{};
        fault_stack_snapshot stack_slot{};
        bool near_null_execute{};
        std::string capture_error{};
        std::optional<size_t> private_execute_vcpu{};
        std::vector<private_execute_memory_row> private_execute_memory{};
    };

    struct io_control_event : observation_event
    {
        std::string device_name{};
        uint32_t code{};
    };

    struct thread_create_event : observation_event
    {
        uint32_t created_thread_id{};
        uint64_t start_address{};
        uint64_t argument{};
        std::vector<std::string> flags{};
    };

    struct thread_terminated_event : observation_event
    {
        uint32_t terminated_thread_id{};
        std::optional<uint32_t> exit_status{};
    };

    struct thread_set_name_event : observation_event
    {
        uint32_t renamed_thread_id{};
        std::string name{};
    };

    struct thread_switch_event : observation_event
    {
        uint32_t previous_thread_id{};
        uint32_t next_thread_id{};
    };

    struct module_load_event : observation_event
    {
        std::string path{};
        uint64_t image_base{};
    };

    struct module_unload_event : observation_event
    {
        std::string path{};
        uint64_t image_base{};
    };

    struct import_read_event : observation_event
    {
        uint64_t resolved_address{};
        std::string import_name{};
        std::string import_module{};
    };

    struct import_write_event : observation_event
    {
        size_t size{};
        uint64_t value{};
        std::string import_name{};
        std::string import_module{};
    };

    struct object_access_event : observation_event
    {
        bool main_access{};
        std::string type_name{};
        uint64_t offset{};
        uint64_t size{};
        std::optional<std::string> member_name{};
    };

    struct environment_access_event : observation_event
    {
        bool main_access{};
        uint64_t offset{};
        uint64_t size{};
    };

    struct function_execution_detail
    {
        std::string label{};
        std::string value{};
    };

    struct function_execution_event : observation_event
    {
        uint64_t call_count{};
        std::string function_name{};
        bool interesting{};
        std::vector<function_execution_detail> details{};
    };

    struct entry_point_execution_event : observation_event
    {
        bool interesting{};
    };

    struct foreign_code_transition_event : observation_event
    {
        std::string function_name{};
        uint64_t function_offset{};
        bool interesting{};
    };

    struct section_first_execute_event : observation_event
    {
        std::string module_name{};
        std::string section_name{};
        uint64_t file_address{};
    };

    struct execution_progress_event : observation_event
    {
        uint64_t elapsed_milliseconds{};
        uint64_t instructions_per_second{};
        std::optional<uint64_t> module_rva{};
        std::optional<double> percent{};  // 0..100 high-water mark through the main image (.text coverage proxy)
    };

    struct rdtsc_event : observation_event
    {
    };

    struct rdtscp_event : observation_event
    {
    };

    struct cpuid_event : observation_event
    {
        uint32_t leaf{};
    };

    struct syscall_event : observation_event
    {
        uint64_t call_count{};
        syscall_classification classification{syscall_classification::regular};
        uint32_t syscall_id{};
        std::string syscall_name{};
        std::optional<uint64_t> caller_rip{};
        std::optional<std::string> caller_module{};
    };

    struct foreign_module_read_event : observation_event
    {
        uint64_t address{};
        size_t size{};
        std::string module_name{};
        std::string region_name{};
    };

    struct executable_read_event : observation_event
    {
        uint64_t address{};
        size_t size{};
        std::string section_name{};
    };

    struct executable_write_event : observation_event
    {
        uint64_t address{};
        size_t size{};
        uint64_t value{};
        std::string section_name{};
    };

    struct fast_fail_caller_code
    {
        uint32_t stack_word_index{};
        uint64_t return_address{};
        std::string module_name{};
        uint64_t module_base{};
        uint64_t module_rva{};
        uint64_t code_base{};
        std::string code_bytes{};
        uint32_t readable_code_bytes{};
    };

    struct fast_fail_event : observation_event
    {
        uint32_t fail_code{};
        std::string rip_module_name{};
        uint64_t rip_module_base{};
        uint64_t rip_module_rva{};
        uint64_t stack_pointer{};
        std::vector<uint64_t> stack_words{};
        // Forty bytes surrounding RIP; each pair is hex or ?? when unreadable.
        uint64_t code_base{};
        std::string code_bytes{};
        uint32_t readable_code_bytes{};
        // Up to four distinct raw stack addresses within loaded-module ranges. These are
        // candidates, not unwound frames. Code is the 16 bytes before each address.
        std::vector<fast_fail_caller_code> caller_code{};
        uint64_t security_cookie_address{};
        uint64_t expected_security_cookie{};
        bool expected_security_cookie_read{};
        // __report_gsfailure saves the incoming RCX at its RSP+0x40.
        uint64_t supplied_security_cookie{};
        bool supplied_security_cookie_read{};
        bool security_cookie_mismatch{};
        // rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, then r8 through r15.
        std::array<uint64_t, 16> gprs{};
        uint64_t gs_base{};
        uint64_t teb_self{};
        uint64_t teb_peb{};
        bool teb_self_read{};
        bool teb_peb_read{};
    };

    using analysis_event =
        std::variant<run_started_event, run_finished_event, run_failed_event, instruction_summary_event, buffered_stdout_event,
                     stdout_chunk_event, suspicious_activity_event, debug_print_call_event, debug_string_event, generic_activity_event,
                     generic_access_event, memory_allocate_event, memory_protect_event, memory_violation_event, io_control_event,
                     thread_create_event, thread_terminated_event, thread_set_name_event, thread_switch_event, module_load_event,
                     module_unload_event, import_read_event, import_write_event, object_access_event, environment_access_event,
                     function_execution_event, entry_point_execution_event, foreign_code_transition_event, section_first_execute_event,
                     execution_progress_event, rdtsc_event, rdtscp_event, cpuid_event, syscall_event, foreign_module_read_event,
                     executable_read_event, executable_write_event, fast_fail_event>;

} // namespace sogen
