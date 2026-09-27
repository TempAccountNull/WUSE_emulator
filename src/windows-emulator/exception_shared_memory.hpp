#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace sogen::detail
{
    // Version 1 wire layout, independent of the reader's compiler.
    struct alignas(8) exception_shared_header
    {
        uint32_t magic{0x58454753}; // "SGEX" in little-endian memory
        uint16_t version{1};
        uint16_t header_bytes{64};
        uint16_t slot_bytes{448};
        uint16_t capacity{128};
        uint32_t flags{};
        int32_t writer_sequence{};
        uint32_t reserved{};
        uint64_t total_published{};
        uint64_t last_observed_ordinal{};
        uint64_t non_debug_total{};
        uint64_t debug_total{};
        uint64_t reserved_tail{};
    };

    struct alignas(8) exception_shared_packet
    {
        uint64_t published_sequence{};
        uint64_t ordinal{};
        uint64_t non_debug_total{};
        uint64_t debug_total{};
        uint32_t status{};
        uint32_t tid{};
        uint32_t vcpu{};
        uint32_t flags{}; // bit 0: retained debug sample
        uint64_t rip{};
        uint64_t info{};
        uint64_t module_base{};
        uint64_t module_rva{};
        std::array<char, 64> module_name{};
        std::array<uint8_t, 16> code_bytes{};
        uint32_t readable_code_bytes{};
        uint32_t readable_stack_words{};
        uint64_t eflags{};
        std::array<uint64_t, 16> gprs{};
        std::array<uint64_t, 16> stack_words{};
        std::array<uint64_t, 2> reserved{};
    };

    static_assert(sizeof(exception_shared_header) == 64);
    static_assert(sizeof(exception_shared_packet) == 448);
    static_assert(offsetof(exception_shared_header, writer_sequence) == 16);
    static_assert(offsetof(exception_shared_header, total_published) == 24);
    static_assert(offsetof(exception_shared_packet, rip) == 48);
    static_assert(offsetof(exception_shared_packet, module_name) == 80);
    static_assert(offsetof(exception_shared_packet, code_bytes) == 144);
    static_assert(offsetof(exception_shared_packet, eflags) == 168);
    static_assert(offsetof(exception_shared_packet, gprs) == 176);
    static_assert(offsetof(exception_shared_packet, stack_words) == 304);

#ifdef _WIN32
    class exception_shared_memory
    {
      public:
        static constexpr size_t capacity = 128;
        static constexpr size_t mapping_size = sizeof(exception_shared_header) + capacity * sizeof(exception_shared_packet);

        explicit exception_shared_memory(std::wstring_view base_name = L"SogenExceptions-")
            : name_(std::wstring(L"Local\\") + std::wstring(base_name) + std::to_wstring(GetCurrentProcessId()))
        {
        }

        exception_shared_memory(const exception_shared_memory&) = delete;
        exception_shared_memory& operator=(const exception_shared_memory&) = delete;

        ~exception_shared_memory()
        {
            if (this->view_)
            {
                UnmapViewOfFile(this->view_);
            }
            if (this->mapping_)
            {
                CloseHandle(this->mapping_);
            }
        }

        // One process may own several emulator instances with separate kernel
        // locks. Serialize their writes while readers use the header seqlock.
        bool publish(exception_shared_packet packet)
        {
            const std::lock_guard lock(this->mutex_);
            if (!this->view_ && !this->open())
            {
                return false;
            }

            auto* const header = static_cast<exception_shared_header*>(this->view_);
            const auto next = header->total_published + 1;
            packet.published_sequence = next;
            // The mapping is process-wide even when emulator instances have
            // separate local ordinals. Assign one wire ordinal and counters.
            packet.ordinal = header->last_observed_ordinal + 1;
            const bool debug_exception = packet.status == 0x80000003u || packet.status == 0x80000004u;
            packet.non_debug_total = header->non_debug_total + (debug_exception ? 0 : 1);
            packet.debug_total = header->debug_total + (debug_exception ? 1 : 0);
            auto* const slots = reinterpret_cast<exception_shared_packet*>(header + 1);
            auto* const sequence = reinterpret_cast<volatile LONG*>(&header->writer_sequence);
            InterlockedIncrement(sequence);
            std::memcpy(&slots[(next - 1) % capacity], &packet, sizeof(packet));
            header->total_published = next;
            header->last_observed_ordinal = packet.ordinal;
            header->non_debug_total = packet.non_debug_total;
            header->debug_total = packet.debug_total;
            InterlockedIncrement(sequence);
            return true;
        }

        // Skipped breakpoint/single-step ordinals remain visible to the reader
        // without copying their registers or altering the existing sample policy.
        bool observe_skipped()
        {
            const std::lock_guard lock(this->mutex_);
            if (!this->view_ && !this->open())
            {
                return false;
            }

            auto* const header = static_cast<exception_shared_header*>(this->view_);
            auto* const sequence = reinterpret_cast<volatile LONG*>(&header->writer_sequence);
            InterlockedIncrement(sequence);
            ++header->last_observed_ordinal;
            ++header->debug_total;
            InterlockedIncrement(sequence);
            return true;
        }

      private:
        bool open()
        {
            auto* const mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                                      static_cast<DWORD>(mapping_size), this->name_.c_str());
            if (!mapping)
            {
                return false;
            }
            if (GetLastError() == ERROR_ALREADY_EXISTS)
            {
                CloseHandle(mapping);
                return false;
            }
            auto* const view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapping_size);
            if (!view)
            {
                CloseHandle(mapping);
                return false;
            }
            auto* const header = static_cast<exception_shared_header*>(view);
            *header = {};
            this->mapping_ = mapping;
            this->view_ = view;
            return true;
        }

        std::wstring name_{};
        std::mutex mutex_{};
        HANDLE mapping_{};
        void* view_{};
    };
#else
    class exception_shared_memory
    {
      public:
        static constexpr size_t capacity = 128;
        static constexpr size_t mapping_size = sizeof(exception_shared_header) + capacity * sizeof(exception_shared_packet);
        explicit exception_shared_memory(std::wstring_view = L"SogenExceptions-") {}
        bool publish(exception_shared_packet) { return false; }
        bool observe_skipped() { return false; }
    };
#endif
    static_assert(exception_shared_memory::mapping_size == 57408);
}
