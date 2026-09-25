#include "../windows-emulator/guest_thread_affinity.hpp"

#include <gtest/gtest.h>
#include <array>
#include <optional>

namespace sogen::test
{
    namespace
    {
        struct candidate
        {
            uint32_t id{};
            std::optional<uint32_t> last_vcpu{};
            bool affinity_deferred_once{};
            bool ready{};
        };

        candidate* dispatch(std::array<candidate, 3>& threads, candidate* active, const bool affinity)
        {
            candidate* selected = nullptr;
            const auto switched = detail::select_next_guest_thread(threads, active, 0, affinity, [&](candidate& thread) {
                if (!thread.ready)
                {
                    return false;
                }
                selected = &thread;
                thread.last_vcpu = 0;
                thread.affinity_deferred_once = false;
                return true;
            });
            EXPECT_EQ(switched, selected != nullptr);
            return selected;
        }
    }

    TEST(GuestThreadAffinity, DisabledPreservesRoundRobinOrder)
    {
        std::array<candidate, 3> threads{{{1, 0, false, true}, {2, 1, false, true}, {3, 0, false, true}}};
        EXPECT_EQ(dispatch(threads, &threads[0], false), &threads[1]);
    }

    TEST(GuestThreadAffinity, PrefersWarmVcpuAndDefersRemoteCandidateOnlyOnce)
    {
        std::array<candidate, 3> threads{{{1, 0, false, true}, {2, 1, false, true}, {3, 0, false, true}}};
        EXPECT_EQ(dispatch(threads, &threads[0], true), &threads[2]);
        EXPECT_TRUE(threads[1].affinity_deferred_once);
        EXPECT_EQ(dispatch(threads, &threads[2], true), &threads[0]);
        EXPECT_EQ(dispatch(threads, &threads[0], true), &threads[1]);
        EXPECT_FALSE(threads[1].affinity_deferred_once);
    }

    TEST(GuestThreadAffinity, RunsRemoteCandidateWhenNoLocalThreadIsReady)
    {
        std::array<candidate, 3> threads{{{1, 0, false, false}, {2, 1, false, true}, {3, 0, false, false}}};
        EXPECT_EQ(dispatch(threads, &threads[0], true), &threads[1]);
    }

    TEST(GuestThreadAffinity, DoesNotRunUnreadyThreadForLocality)
    {
        std::array<candidate, 3> threads{{{1, 0, false, false}, {2, 0, false, false}, {3, 1, true, true}}};
        EXPECT_EQ(dispatch(threads, &threads[0], true), &threads[2]);
    }
}
