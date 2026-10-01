#pragma once

#include "guest_inspection_protocol.hpp"
#include "guest_sampling_admission.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sogen::detail
{
    struct inspection_owner
    {
        uint32_t cpu{};
        uint32_t tid{};
        bool running{};
        bool terminated{};
        uint32_t cpu_count{UINT32_MAX};
    };

    inline bool inspection_valid_request(const guest_inspection_packet& packet)
    {
        switch (static_cast<inspection_operation>(packet.operation))
        {
        case inspection_operation::address:
            return packet.limit == 1 && packet.address < 0x7fffffff0000ULL;
        case inspection_operation::regions:
            return packet.limit >= 1 && packet.limit <= 64 && packet.address < 0x7fffffff0000ULL;
        case inspection_operation::context:
            return packet.limit <= 128;
        default:
            return false;
        }
    }

    class inspection_service_state
    {
      public:
        explicit inspection_service_state(const inspection_identity identity,
                                          guest_sampling_admission& admission = shared_guest_sampling_admission())
            : identity_(identity),
              admission_(admission)
        {
        }

        const inspection_identity& identity() const
        {
            return this->identity_;
        }

        void set_generation(const uint64_t generation)
        {
            this->identity_.generation = generation;
        }

        template <typename Reader>
        bool service(guest_inspection_packet& packet, const inspection_owner owner, const uint64_t now_ns, Reader&& reader)
        {
            if (packet.magic != guest_inspection_packet::magic_value || packet.version != guest_inspection_packet::version_value ||
                packet.header_size != 128 || packet.packet_size != sizeof(packet) || !packet.request_id ||
                packet.request_id == packet.response_id || packet.request_id == this->last_response_id_)
            {
                return false;
            }
            const bool identity_matches = packet.expected_pid == this->identity_.pid && packet.expected_birth == this->identity_.birth &&
                                          packet.expected_generation == this->identity_.generation;
            const bool context = packet.operation == static_cast<uint32_t>(inspection_operation::context);
            const bool valid = inspection_valid_request(packet) && (!context || packet.requested_cpu < owner.cpu_count);
            if (identity_matches && valid && context && packet.requested_cpu != owner.cpu)
            {
                return false;
            }
            std::memset(packet.payload.data(), 0, sizeof(packet.payload));
            packet.producer_pid = this->identity_.pid;
            packet.producer_birth = this->identity_.birth;
            packet.producer_generation = this->identity_.generation;
            packet.actual_cpu = UINT32_MAX;
            packet.actual_tid = 0;
            packet.next_cursor = 0;
            packet.sample_sequence = 0;
            packet.steady_clock_ns = 0;
            packet.status = static_cast<uint32_t>(inspection_status::invalid);
            if (!identity_matches)
            {
                packet.status = static_cast<uint32_t>(inspection_status::identity_mismatch);
            }
            else if (valid)
            {
                if (context &&
                    (owner.running || !owner.tid || owner.terminated || (packet.requested_tid && packet.requested_tid != owner.tid)))
                {
                    packet.status = static_cast<uint32_t>(inspection_status::unavailable);
                }
                else if (!this->admission_.try_admit(now_ns))
                {
                    packet.status = static_cast<uint32_t>(inspection_status::rate_limited);
                }
                else
                {
                    packet.sample_sequence = ++this->sample_sequence_;
                    packet.steady_clock_ns = now_ns;
                    if (context)
                    {
                        packet.actual_cpu = owner.cpu;
                        packet.actual_tid = owner.tid;
                    }
                    try
                    {
                        reader(packet);
                    }
                    catch (...)
                    {
                        std::memset(packet.payload.data(), 0, sizeof(packet.payload));
                        packet.next_cursor = 0;
                        packet.status = static_cast<uint32_t>(inspection_status::exception);
                    }
                }
            }
            this->last_response_id_ = packet.request_id;
            packet.response_id = packet.request_id;
            return true;
        }

      private:
        inspection_identity identity_{};
        guest_sampling_admission& admission_;
        uint64_t last_response_id_{};
        uint64_t sample_sequence_{};
    };

#ifdef _WIN32
    class guest_inspection_shared_memory
    {
      public:
        guest_inspection_shared_memory()
            : state_(make_identity())
        {
            const auto& identity = this->state_.identity();
            if (!identity.birth)
            {
                return;
            }
            const auto suffix = std::to_wstring(identity.pid) + L"-" + std::to_wstring(identity.birth);
            const auto mapping_name = L"Local\\SogenGuestInspection-" + suffix;
            const auto mutex_name = L"Local\\SogenGuestInspectionMutex-" + suffix;
            this->mapping_ =
                CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(guest_inspection_packet), mapping_name.c_str());
            if (!this->mapping_ || GetLastError() == ERROR_ALREADY_EXISTS)
            {
                this->close();
                return;
            }
            this->mutex_ = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
            this->packet_ = static_cast<guest_inspection_packet*>(
                MapViewOfFile(this->mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(guest_inspection_packet)));
            if (!this->mutex_ || !this->packet_)
            {
                this->close();
                return;
            }
            new (this->packet_) guest_inspection_packet{};
            this->packet_->producer_pid = identity.pid;
            this->packet_->producer_birth = identity.birth;
            this->packet_->producer_generation = identity.generation;
        }

        guest_inspection_shared_memory(const guest_inspection_shared_memory&) = delete;
        guest_inspection_shared_memory& operator=(const guest_inspection_shared_memory&) = delete;

        ~guest_inspection_shared_memory()
        {
            this->close();
        }

        void advance_generation()
        {
            this->state_.set_generation(next_generation());
        }

        template <typename Reader>
        void service(const inspection_owner owner, Reader&& reader) noexcept
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
            const auto now = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            this->state_.service(*this->packet_, owner, now, std::forward<Reader>(reader));
            ReleaseMutex(this->mutex_);
        }

      private:
        static uint64_t next_generation()
        {
            static std::atomic<uint64_t> sequence{0};
            return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        static inspection_identity make_identity()
        {
            FILETIME birth{}, exit{}, kernel{}, user{};
            if (!GetProcessTimes(GetCurrentProcess(), &birth, &exit, &kernel, &user))
            {
                return {};
            }
            return {GetCurrentProcessId(), (uint64_t{birth.dwHighDateTime} << 32) | birth.dwLowDateTime, next_generation()};
        }

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

        inspection_service_state state_;
        HANDLE mapping_{};
        HANDLE mutex_{};
        guest_inspection_packet* packet_{};
    };
#endif
}
