#pragma once

#include <span>
#include <vector>
#include <cstdint>
#include <memory>
#include <ostream>

namespace sogen
{

    namespace utils::compression
    {
        namespace zstd
        {
            class stream_compressor
            {
              public:
                stream_compressor(std::ostream& output, uint64_t content_size, int compression_level = 8);
                ~stream_compressor();
                stream_compressor(const stream_compressor&) = delete;
                stream_compressor& operator=(const stream_compressor&) = delete;

                void write(std::span<const std::byte> data);
                void finish();

              private:
                struct state;
                std::unique_ptr<state> state_;
            };

            std::vector<std::byte> compress(std::span<const std::byte> data, int compression_level = 8);
            std::vector<std::byte> decompress(std::span<const std::byte> data);
        }
    }
} // namespace sogen
