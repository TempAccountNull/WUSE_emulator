#include <gtest/gtest.h>
#include <async_handler.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

namespace sogen::test
{
    TEST(GdbAsyncHandler, ImmediateWorkerStopDoesNotStrandRun)
    {
        std::atomic_bool recovery{false};
        gdb_stub::async_handler worker{[&](std::atomic_bool& should_run) {
            if (recovery)
            {
                while (should_run)
                {
                    std::this_thread::yield();
                }
            }
            else
            {
                should_run = false;
            }
        }};

        for (size_t attempt = 0; attempt < 64; ++attempt)
        {
            auto running = std::async(std::launch::async, [&] { worker.run(); });
            const auto status = running.wait_for(2s);
            if (status != std::future_status::ready)
            {
                recovery = true;
                worker.run();
                running.get();
                worker.pause();
                FAIL() << "run missed a completed worker at attempt " << attempt;
            }
            running.get();
            worker.pause();
        }
    }

    TEST(GdbAsyncHandler, PauseWaitsForWorkerToReturn)
    {
        std::promise<void> cancelling;
        std::promise<void> finish;
        auto finish_requested = finish.get_future();
        gdb_stub::async_handler worker{[&](std::atomic_bool& should_run) {
            while (should_run)
            {
                std::this_thread::yield();
            }
            cancelling.set_value();
            finish_requested.wait();
        }};
        worker.run();
        auto paused = std::async(std::launch::async, [&] { worker.pause(); });
        const auto status = cancelling.get_future().wait_for(2s);
        EXPECT_EQ(status, std::future_status::ready);
        EXPECT_EQ(paused.wait_for(0s), std::future_status::timeout);
        finish.set_value();
        EXPECT_EQ(paused.wait_for(2s), std::future_status::ready);
        paused.get();
        EXPECT_FALSE(worker.is_running());
    }
}
