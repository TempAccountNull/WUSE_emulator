#include "../windows-analyzer/analysis.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sogen::test
{
    namespace
    {
        class rendezvous_reporter final : public analysis_reporter
        {
          public:
            void report(const analysis_event&) override
            {
                std::unique_lock lock{mutex_};
                const auto pair_end = (calls_ / 2 + 1) * 2;
                ++calls_;
                if (calls_ == pair_end)
                {
                    cv_.notify_all();
                }
                else
                {
                    EXPECT_TRUE(cv_.wait_for(lock, std::chrono::seconds(5), [&] { return calls_ >= pair_end; }));
                }
            }

          private:
            std::mutex mutex_{};
            std::condition_variable cv_{};
            uint32_t calls_{};
        };
    } // namespace

    TEST(AnalysisEventOverlap, CountsConcurrentReporterEntryAndCapsPackets)
    {
        rendezvous_reporter reporter{};
        analysis_context context{.reporters = {&reporter}};
        context.event_overlap_probe.set_enabled(true);
        const analysis_event first = object_access_event{};
        const analysis_event second = environment_access_event{};

        for (int pair = 0; pair < 6; ++pair)
        {
            std::thread a{[&] { context.emit_event(first); }};
            std::thread b{[&] { context.emit_event(second); }};
            a.join();
            b.join();
        }

        const auto stats = context.event_overlap_probe.read_snapshot();
        EXPECT_EQ(stats.overlaps, 6);
        EXPECT_EQ(stats.lines_emitted, 4);
        EXPECT_GE(stats.max_active, 2U);
        EXPECT_NE(stats.first_host_tid, 0U);
        EXPECT_NE(stats.second_host_tid, 0U);
        EXPECT_NE(stats.first_host_tid, stats.second_host_tid);
        EXPECT_TRUE(stats.first_type == "object_access" || stats.first_type == "environment_access");
        EXPECT_TRUE(stats.second_type == "object_access" || stats.second_type == "environment_access");
        EXPECT_NE(stats.first_type, stats.second_type);
    }
} // namespace sogen::test
