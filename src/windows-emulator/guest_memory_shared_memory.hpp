#pragma once
#include "guest_sampling_admission.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

namespace sogen::detail
{
    struct guest_memory_packet
    {
        static constexpr uint32_t magic_value = 0x4d474753;
        static constexpr uint16_t version_value = 1;
        static constexpr size_t max_read = 4096;

        uint32_t magic{magic_value};
        uint16_t version{version_value};
        uint16_t packet_size{sizeof(guest_memory_packet)};
        uint64_t request_id{};
        uint64_t address{};
        uint32_t length{};
        uint32_t reserved{};
        uint64_t response_id{};
        uint32_t status{};
        uint32_t bytes_read{};
        uint64_t module_base{};
        uint64_t module_size{};
        char module_name[96]{};
        uint8_t data[max_read]{};
    };

    static_assert(sizeof(guest_memory_packet) == 4256);

#ifdef _WIN32
    class guest_memory_shared_memory
    {
      public:
        guest_memory_shared_memory()
        {
            const auto suffix = std::to_wstring(GetCurrentProcessId());
            const auto mapping_name = L"Local\\SogenGuestMemory-" + suffix;
            const auto mutex_name = L"Local\\SogenGuestMemoryMutex-" + suffix;
            this->mapping_ =
                CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(guest_memory_packet), mapping_name.c_str());
            if (!this->mapping_ || GetLastError() == ERROR_ALREADY_EXISTS)
            {
                this->close();
                return;
            }
            this->mutex_ = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
            this->packet_ =
                static_cast<guest_memory_packet*>(MapViewOfFile(this->mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(guest_memory_packet)));
            if (!this->mutex_ || !this->packet_)
            {
                this->close();
                return;
            }
            new (this->packet_) guest_memory_packet{};
        }

        guest_memory_shared_memory(const guest_memory_shared_memory&) = delete;
        guest_memory_shared_memory& operator=(const guest_memory_shared_memory&) = delete;

        ~guest_memory_shared_memory()
        {
            this->close();
        }

        template <typename Reader>
        void service(Reader&& reader) noexcept
        {
            if (!this->packet_ || !this->mutex_)
            {
                return;
            }
            const auto wait = WaitForSingleObject(this->mutex_, 0);
            if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
            {
                return;
            }

            auto& packet = *this->packet_;
            if (packet.magic == guest_memory_packet::magic_value && packet.version == guest_memory_packet::version_value &&
                packet.packet_size == sizeof(packet) && packet.request_id && packet.request_id != packet.response_id)
            {
                packet.status = 2;
                packet.bytes_read = 0;
                packet.module_base = 0;
                packet.module_size = 0;
                std::memset(packet.module_name, 0, sizeof(packet.module_name));
                std::memset(packet.data, 0, sizeof(packet.data));
                if (packet.length > 0 && packet.length <= guest_memory_packet::max_read && packet.address <= UINT64_MAX - packet.length)
                {
                    const auto now = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                    if (!shared_guest_sampling_admission().try_admit(now))
                    {
                        packet.status = 4;
                    }
                    else
                    {
                        try
                        {
                            reader(packet.address, packet.length, packet);
                        }
                        catch (...)
                        {
                            packet.status = 3;
                            packet.bytes_read = 0;
                        }
                    }
                }
                packet.response_id = packet.request_id;
            }
            ReleaseMutex(this->mutex_);
        }

      private:
        void close()
        {
            if (this->packet_)
            {
                UnmapViewOfFile(this->packet_);
                this->packet_ = nullptr;
            }
            if (this->mutex_)
            {
                CloseHandle(this->mutex_);
                this->mutex_ = nullptr;
            }
            if (this->mapping_)
            {
                CloseHandle(this->mapping_);
                this->mapping_ = nullptr;
            }
        }

        HANDLE mapping_{};
        HANDLE mutex_{};
        guest_memory_packet* packet_{};
    };
#endif
}
