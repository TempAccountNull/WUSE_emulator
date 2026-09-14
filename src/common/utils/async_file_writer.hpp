#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace sogen::utils
{
    class async_file_writer
    {
      public:
        explicit async_file_writer(const std::filesystem::path& path)
            : file_(path, std::ios::binary | std::ios::out | std::ios::trunc)
        {
            if (!this->file_)
            {
                throw std::runtime_error("Failed to open output file: " + path.string());
            }
            this->pending_.reserve(BUFFER_LIMIT);
            this->worker_ = std::thread([this] { this->run(); });
        }

        ~async_file_writer()
        {
            {
                const std::scoped_lock lock(this->mutex_);
                this->stopping_ = true;
            }
            this->changed_.notify_all();
            this->worker_.join();
        }

        async_file_writer(const async_file_writer&) = delete;
        async_file_writer& operator=(const async_file_writer&) = delete;
        async_file_writer(async_file_writer&&) = delete;
        async_file_writer& operator=(async_file_writer&&) = delete;

        void write(std::string_view data)
        {
            std::unique_lock lock(this->mutex_);
            while (!data.empty())
            {
                this->changed_.wait(lock, [this] { return this->failure_ || this->pending_.size() < BUFFER_LIMIT; });
                this->check_failure();
                const auto count = std::min(data.size(), BUFFER_LIMIT - this->pending_.size());
                this->pending_.append(data.substr(0, count));
                data.remove_prefix(count);
                if (this->pending_.size() >= FLUSH_SIZE)
                {
                    this->changed_.notify_all();
                }
            }
        }

        void flush()
        {
            std::unique_lock lock(this->mutex_);
            this->check_failure();
            const auto requested = ++this->requested_flush_;
            this->changed_.notify_all();
            this->changed_.wait(lock, [this, requested] { return this->failure_ || this->completed_flush_ >= requested; });
            this->check_failure();
        }

      private:
        static constexpr size_t BUFFER_LIMIT = 1024 * 1024;
        static constexpr size_t FLUSH_SIZE = 64 * 1024;
        std::ofstream file_;
        std::mutex mutex_;
        std::condition_variable changed_;
        std::string pending_;
        uint64_t requested_flush_{};
        uint64_t completed_flush_{};
        bool stopping_{};
        std::exception_ptr failure_;
        std::thread worker_;

        void check_failure() const
        {
            if (this->failure_)
            {
                std::rethrow_exception(this->failure_);
            }
        }

        void run()
        {
            try
            {
                std::string batch;
                batch.reserve(BUFFER_LIMIT);
                std::unique_lock lock(this->mutex_);
                while (true)
                {
                    this->changed_.wait_for(lock, std::chrono::milliseconds(250), [this] {
                        return this->stopping_ || this->pending_.size() >= FLUSH_SIZE || this->requested_flush_ != this->completed_flush_;
                    });
                    const auto requested = this->requested_flush_;
                    const auto stopping = this->stopping_;
                    batch.swap(this->pending_);
                    lock.unlock();
                    this->changed_.notify_all();
                    if (!batch.empty())
                    {
                        this->file_.write(batch.data(), static_cast<std::streamsize>(batch.size()));
                    }
                    this->file_.flush();
                    if (!this->file_)
                    {
                        throw std::runtime_error("Failed to write output file");
                    }
                    batch.clear();
                    lock.lock();
                    this->completed_flush_ = requested;
                    this->changed_.notify_all();
                    if (stopping)
                    {
                        break;
                    }
                }
            }
            catch (...)
            {
                const std::scoped_lock lock(this->mutex_);
                this->failure_ = std::current_exception();
                this->changed_.notify_all();
            }
        }
    };
}
