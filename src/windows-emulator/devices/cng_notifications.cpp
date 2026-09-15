#include "../std_include.hpp"
#include "cng_notifications.hpp"
#include "../windows_emulator.hpp"

namespace sogen
{
    namespace
    {
        NTSTATUS validate_event(windows_emulator& emu, const handle value)
        {
            if (emu.process.events.get(value))
            {
                return STATUS_SUCCESS;
            }
            auto* store = emu.process.get_handle_store(value);
            return store && store->contains(value) ? STATUS_OBJECT_TYPE_MISMATCH : STATUS_INVALID_HANDLE;
        }
    }

    NTSTATUS cng_notifications::subscribe(windows_emulator& emu, const handle event)
    {
        const auto status = validate_event(emu, event);
        if (status != STATUS_SUCCESS)
        {
            return status;
        }
        if (std::ranges::find(events_, event) != events_.end())
        {
            return STATUS_OBJECT_NAME_COLLISION;
        }
        events_.push_back(event);
        emu.process.events.duplicate(event);
        return STATUS_SUCCESS;
    }

    NTSTATUS cng_notifications::unsubscribe(windows_emulator& emu, const handle event)
    {
        const auto status = validate_event(emu, event);
        if (status != STATUS_SUCCESS)
        {
            return status;
        }
        const auto found = std::ranges::find(events_, event);
        if (found != events_.end())
        {
            events_.erase(found);
            emu.process.events.erase(event);
        }
        return STATUS_SUCCESS;
    }

    void cng_notifications::publish(windows_emulator& emu) const
    {
        for (const auto handle : events_)
        {
            if (auto* event = emu.process.events.get(handle))
            {
                event->signaled = true;
            }
        }
    }

    void cng_notifications::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write<uint64_t>(0x31544e4556474e43);
        buffer.write_vector(events_);
    }

    void cng_notifications::deserialize(utils::buffer_deserializer& buffer, windows_emulator& emu)
    {
        events_.clear();
        if (!buffer.get_remaining_size())
        {
            return;
        }
        if (buffer.read<uint64_t>() != 0x31544e4556474e43)
        {
            throw std::runtime_error("Invalid CNG notification snapshot extension");
        }
        buffer.read_vector(events_);
        std::vector<handle> seen;
        for (const auto event : events_)
        {
            if (!emu.process.events.get(event) || std::ranges::find(seen, event) != seen.end())
            {
                throw std::runtime_error("Invalid saved CNG event reference");
            }
            seen.push_back(event);
        }
    }
}
