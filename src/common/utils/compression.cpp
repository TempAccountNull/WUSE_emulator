#include "compression.hpp"

#include <zstd.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <algorithm>

namespace sogen
{

    namespace utils::compression
    {
        namespace zstd
        {
            namespace
            {
                size_t check_result(const size_t result)
                {
                    if (ZSTD_isError(result))
                    {
                        throw std::runtime_error(std::string("Zstd compression failed: ") + ZSTD_getErrorName(result));
                    }
                    return result;
                }
            }

            struct stream_compressor::state
            {
                std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context{ZSTD_createCCtx(), ZSTD_freeCCtx};
                std::vector<std::byte> buffer = std::vector<std::byte>(ZSTD_CStreamOutSize());
                std::vector<std::byte> input_buffer = std::vector<std::byte>(0x10000);
                size_t input_size{};
                std::ostream& output;
                uint64_t expected{};
                uint64_t written{};
                bool finished{};

                state(std::ostream& stream, const uint64_t size, const int level)
                    : output(stream),
                      expected(size)
                {
                    if (!this->context)
                    {
                        throw std::bad_alloc();
                    }
                    check_result(ZSTD_CCtx_setParameter(this->context.get(), ZSTD_c_compressionLevel, level));
                    check_result(ZSTD_CCtx_setPledgedSrcSize(this->context.get(), size));
                }

                size_t compress(ZSTD_inBuffer& input, const ZSTD_EndDirective directive)
                {
                    ZSTD_outBuffer target{this->buffer.data(), this->buffer.size(), 0};
                    const auto remaining = check_result(ZSTD_compressStream2(this->context.get(), &target, &input, directive));
                    if (target.pos)
                    {
                        this->output.write(reinterpret_cast<const char*>(this->buffer.data()), static_cast<std::streamsize>(target.pos));
                        if (!this->output)
                        {
                            throw std::runtime_error("Failed to write compressed snapshot data");
                        }
                    }
                    return remaining;
                }
            };

            stream_compressor::stream_compressor(std::ostream& output, const uint64_t content_size, const int compression_level)
                : state_(std::make_unique<state>(output, content_size, compression_level))
            {
            }

            stream_compressor::~stream_compressor() = default;

            void stream_compressor::write(const std::span<const std::byte> data)
            {
                auto& s = *this->state_;
                if (s.finished || data.size() > s.expected - s.written)
                {
                    throw std::runtime_error("Compressed input exceeds declared content size or frame is finished");
                }
                for (size_t offset = 0; offset < data.size();)
                {
                    const auto count = std::min(s.input_buffer.size() - s.input_size, data.size() - offset);
                    memcpy(s.input_buffer.data() + s.input_size, data.data() + offset, count);
                    s.input_size += count;
                    offset += count;
                    if (s.input_size == s.input_buffer.size())
                    {
                        ZSTD_inBuffer input{s.input_buffer.data(), s.input_size, 0};
                        while (input.pos < input.size)
                        {
                            s.compress(input, ZSTD_e_continue);
                        }
                        s.input_size = 0;
                    }
                }
                s.written += data.size();
            }

            void stream_compressor::finish()
            {
                auto& s = *this->state_;
                if (s.finished)
                {
                    return;
                }
                if (s.written != s.expected)
                {
                    throw std::runtime_error("Compressed input does not match declared content size");
                }
                ZSTD_inBuffer input{s.input_buffer.data(), s.input_size, 0};
                while (s.compress(input, ZSTD_e_end) != 0)
                {
                }
                s.input_size = 0;
                s.finished = true;
            }

            std::vector<std::byte> decompress(const std::span<const std::byte> data)
            {
                const auto decompressed_size = ZSTD_getFrameContentSize(data.data(), data.size());

                if (decompressed_size == ZSTD_CONTENTSIZE_ERROR || decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN)
                {
                    return {};
                }

                std::vector<std::byte> buffer(static_cast<size_t>(decompressed_size));

                const auto result = ZSTD_decompress(buffer.data(), buffer.size(), data.data(), data.size());

                if (ZSTD_isError(result))
                {
                    return {};
                }

                return buffer;
            }

            std::vector<std::byte> compress(const std::span<const std::byte> data, const int compression_level)
            {
                const auto max_size = ZSTD_compressBound(data.size());
                std::vector<std::byte> result(max_size);

                const auto compressed_size = ZSTD_compress(result.data(), max_size, data.data(), data.size(), compression_level);

                if (ZSTD_isError(compressed_size))
                {
                    return {};
                }

                result.resize(compressed_size);
                return result;
            }
        }
    }
} // namespace sogen
