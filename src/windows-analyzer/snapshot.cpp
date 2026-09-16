#include "snapshot.hpp"

#include <utils/io.hpp>
#include <utils/compression.hpp>
#include <utils/finally.hpp>
#include <fstream>
#include <chrono>
#include <atomic>

#ifdef OS_WINDOWS
#include <windows.h>
#endif

namespace sogen
{

    namespace snapshot
    {
        namespace
        {
            struct snapshot_header
            {
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
                char magic[4] = {'S', 'N', 'A', 'P'};
                uint32_t version{1};
            };

            static_assert(sizeof(snapshot_header) == 8);

            std::span<const std::byte> validate_header(const std::span<const std::byte> snapshot)
            {
                snapshot_header header{};
                constexpr snapshot_header default_header{};

                if (snapshot.size() < sizeof(header))
                {
                    throw std::runtime_error("Snapshot is too small");
                }

                memcpy(&header, snapshot.data(), sizeof(header));

                if (memcmp(default_header.magic, header.magic, sizeof(header.magic)) != 0)
                {
                    throw std::runtime_error("Invalid snapshot");
                }

                if (default_header.version != header.version)
                {
                    throw std::runtime_error("Unsupported snapshot version: " + std::to_string(header.version) +
                                             "(needed: " + std::to_string(default_header.version) + ")");
                }

                return snapshot.subspan(sizeof(header));
            }

            std::vector<std::byte> get_compressed_emulator_state(const windows_emulator& win_emu)
            {
                utils::buffer_serializer serializer{};
                win_emu.serialize(serializer);

                return utils::compression::zstd::compress(serializer.get_buffer());
            }

            std::vector<std::byte> get_decompressed_emulator_state(const std::span<const std::byte> snapshot)
            {
                const auto data = validate_header(snapshot);
                return utils::compression::zstd::decompress(data);
            }

            std::filesystem::path create_snapshot_staging_directory(const std::filesystem::path& snapshot_file)
            {
                if (snapshot_file.has_parent_path())
                {
                    std::filesystem::create_directories(snapshot_file.parent_path());
                }
                static std::atomic_uint64_t sequence{};
                const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
                for (size_t attempt = 0; attempt < 100; ++attempt)
                {
                    auto path = snapshot_file;
                    path += ".writing-" + std::to_string(stamp) + "-" + std::to_string(sequence++);
                    if (std::filesystem::create_directory(path))
                    {
                        return path;
                    }
                }
                throw std::runtime_error("Cannot create snapshot staging directory");
            }

            void publish_snapshot(const std::filesystem::path& source, const std::filesystem::path& target)
            {
#ifdef OS_WINDOWS
                if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot publish snapshot");
                }
#else
                std::filesystem::rename(source, target);
#endif
            }

            std::string get_main_executable_name(const windows_emulator& win_emu)
            {
                const auto* exe = win_emu.mod_manager.executable;
                if (exe)
                {
                    return std::filesystem::path(exe->name).stem().string();
                }

                return "process";
            }
        }

        std::vector<std::byte> create_emulator_snapshot(const windows_emulator& win_emu)
        {
            const auto state = get_compressed_emulator_state(win_emu);

            snapshot_header header{};
            std::span header_span(reinterpret_cast<const std::byte*>(&header), sizeof(header));

            std::vector<std::byte> snapshot{};
            snapshot.reserve(header_span.size() + state.size());
            snapshot.assign(header_span.begin(), header_span.end());
            snapshot.insert(snapshot.end(), state.begin(), state.end());

            return snapshot;
        }

        std::filesystem::path write_emulator_snapshot(const windows_emulator& win_emu, const bool log)
        {
            const std::filesystem::path snapshot_file = get_main_executable_name(win_emu) + "-" + std::to_string(time(nullptr)) + ".snap";
            return write_emulator_snapshot(win_emu, snapshot_file, log);
        }

        std::filesystem::path write_emulator_snapshot(const windows_emulator& win_emu, const std::filesystem::path& snapshot_file,
                                                      const bool log)
        {
            if (log)
            {
                win_emu.log.log("Writing snapshot to %s...\n", snapshot_file.string().c_str());
            }

            auto count = utils::buffer_serializer::counting();
            win_emu.serialize(count);

            const auto staging = create_snapshot_staging_directory(snapshot_file);
            const auto staged_file = staging / "snapshot";
            const auto cleanup = utils::finally([&] {
                std::error_code error{};
                std::filesystem::remove(staged_file, error);
                std::filesystem::remove(staging, error);
            });

            std::ofstream stream(staged_file, std::ios::binary | std::ios::trunc);
            stream.exceptions(std::ios::badbit | std::ios::failbit);
            const snapshot_header header{};
            stream.write(reinterpret_cast<const char*>(&header), sizeof(header));

            utils::compression::zstd::stream_compressor compressor{stream, count.size()};
            utils::buffer_serializer serializer{[&](const std::span<const std::byte> data) { compressor.write(data); }};
            win_emu.serialize(serializer);
            compressor.finish();
            stream.flush();
            stream.close();
            publish_snapshot(staged_file, snapshot_file);

            return snapshot_file;
        }

        void load_emulator_snapshot(windows_emulator& win_emu, const std::span<const std::byte> snapshot)
        {
            const auto data = get_decompressed_emulator_state(snapshot);

            utils::buffer_deserializer deserializer{data};
            win_emu.deserialize(deserializer);
        }

        void load_emulator_snapshot(windows_emulator& win_emu, const std::filesystem::path& snapshot_file)
        {
            std::vector<std::byte> data{};
            if (!utils::io::read_file(snapshot_file, &data))
            {
                throw std::runtime_error("Failed to read snapshot file: " + snapshot_file.string());
            }

            load_emulator_snapshot(win_emu, data);
        }
    }

} // namespace sogen
