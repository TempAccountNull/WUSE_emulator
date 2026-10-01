#pragma once

#include <cstdint>
#include <mutex>

namespace sogen::detail
{
    class guest_sampling_admission
    {
      public:
        bool try_admit(const uint64_t now_ns)
        {
            std::unique_lock lock(this->mutex_, std::try_to_lock);
            if (!lock.owns_lock() || (this->seen_ && (now_ns < this->last_ns_ || now_ns - this->last_ns_ < 50000000)))
            {
                return false;
            }
            this->seen_ = true;
            this->last_ns_ = now_ns;
            return true;
        }

      private:
        std::mutex mutex_{};
        uint64_t last_ns_{};
        bool seen_{};
    };

    inline guest_sampling_admission& shared_guest_sampling_admission()
    {
        static guest_sampling_admission admission;
        return admission;
    }
}
