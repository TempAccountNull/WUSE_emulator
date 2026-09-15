#pragma once

#include "../handles.hpp"
#include "../std_include.hpp"

namespace sogen
{
    class windows_emulator;

    class cng_notifications
    {
      public:
        NTSTATUS subscribe(windows_emulator& emu, handle event);
        NTSTATUS unsubscribe(windows_emulator& emu, handle event);
        void publish(windows_emulator& emu) const;
        void serialize(utils::buffer_serializer& buffer) const;
        void deserialize(utils::buffer_deserializer& buffer, windows_emulator& emu);

      private:
        std::vector<handle> events_;
    };
}
