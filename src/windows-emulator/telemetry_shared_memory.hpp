#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace sogen::detail
{
#ifdef _WIN32
    class telemetry_shared_memory
    {
      public:
        static constexpr size_t mapping_size = 64 * 1024;
        static constexpr size_t header_size = sizeof(uint32_t) * 2;
        static constexpr size_t max_payload_size = mapping_size - header_size;

        explicit telemetry_shared_memory(const std::wstring_view base_name = L"SogenTelemetry-")
            : name_(std::wstring(L"Local\\") + std::wstring(base_name) + std::to_wstring(GetCurrentProcessId()))
        {
        }

        telemetry_shared_memory(const telemetry_shared_memory&) = delete;
        telemetry_shared_memory& operator=(const telemetry_shared_memory&) = delete;

        ~telemetry_shared_memory()
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

        bool publish(const std::string_view json)
        {
            if (json.size() > max_payload_size)
            {
                return false;
            }

            const std::lock_guard lock(this->mutex_);
            if (!this->view_ && !this->open())
            {
                return false;
            }

            auto* const bytes = static_cast<unsigned char*>(this->view_);
            auto* const sequence = reinterpret_cast<volatile LONG*>(bytes);
            auto* const length = reinterpret_cast<volatile LONG*>(bytes + sizeof(uint32_t));
            InterlockedIncrement(sequence);
            std::memcpy(bytes + header_size, json.data(), json.size());
            InterlockedExchange(length, static_cast<LONG>(json.size()));
            InterlockedIncrement(sequence);
            return true;
        }

      private:
        bool open()
        {
            auto* const mapping =
                CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(mapping_size), this->name_.c_str());
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
    class telemetry_shared_memory
    {
      public:
        explicit telemetry_shared_memory(std::wstring_view = L"SogenTelemetry-")
        {
        }

        bool publish(std::string_view)
        {
            return false;
        }
    };
#endif
}
