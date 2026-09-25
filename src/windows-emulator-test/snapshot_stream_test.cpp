#include "emulation_test_utils.hpp"
#include "../windows-analyzer/snapshot.hpp"
#include "../windows-analyzer/analysis_reporter.hpp"
#include "../windows-analyzer/jsonl_reporter.hpp"
#include <utils/compression.hpp>
#include <utils/io.hpp>
#include <utils/finally.hpp>
#include <sstream>
#include <fstream>
#include <zstd.h>

namespace sogen::test
{
    namespace
    {
        std::vector<std::byte> stream_bytes(const std::string& value)
        {
            const auto* first = reinterpret_cast<const std::byte*>(value.data());
            return {first, first + value.size()};
        }

        class rejecting_output : public std::streambuf
        {
            std::streamsize xsputn(const char*, std::streamsize) override
            {
                return 0;
            }
        };

        class snapshot_test_clock : public utils::clock
        {
          public:
            explicit snapshot_test_clock(const steady_time_point now)
                : now_(now)
            {
            }

            steady_time_point steady_now() override
            {
                return this->now_;
            }

          private:
            steady_time_point now_;
        };
    }

    TEST(SnapshotStream, ChunkingPreservesLegacyMarkersAndZeroLengthFields)
    {
        for (const size_t length : {0u, 1u, 255u, 256u, 65535u, 65536u, 65537u, 262147u})
        {
            std::vector<std::byte> data(length);
            for (size_t i = 0; i < length; ++i)
            {
                data[i] = static_cast<std::byte>(i * 13 + 7);
            }
            utils::buffer_serializer legacy{};
            legacy.write<uint32_t>(0x12345678);
            legacy.write(data.data(), data.size());
            legacy.write<std::string>("after payload");

            std::vector<std::byte> output;
            utils::buffer_serializer streamed{[&](const std::span<const std::byte> chunk) {
                EXPECT_LE(chunk.size(), 65536u);
                output.insert(output.end(), chunk.begin(), chunk.end());
            }};
            streamed.write<uint32_t>(0x12345678);
            streamed.write_chunked(length, [&](const size_t offset, const std::span<std::byte> chunk) {
                ASSERT_LE(offset + chunk.size(), data.size());
                memcpy(chunk.data(), data.data() + offset, chunk.size());
            });
            streamed.write<std::string>("after payload");
            EXPECT_EQ(output, legacy.get_buffer());
            EXPECT_EQ(streamed.size(), legacy.size());
            EXPECT_THROW(streamed.get_buffer(), std::logic_error);
            utils::buffer_deserializer read{output};
            EXPECT_EQ(read.read<uint32_t>(), 0x12345678u);
            const auto restored = read.read_data(length);
            EXPECT_EQ(std::vector<std::byte>(restored.begin(), restored.end()), data);
            EXPECT_EQ(read.read<std::string>(), "after payload");
            EXPECT_EQ(read.get_remaining_size(), 0u);
        }
    }

    TEST(SnapshotStream, CountingDoesNotReadGuestMemoryAndChecksOverflow)
    {
        auto count = utils::buffer_serializer::counting();
        const size_t huge = size_t{5} * 1024 * 1024 * 1024;
        count.write<uint32_t>(17);
        count.write_chunked(huge, [](size_t, std::span<std::byte>) { ADD_FAILURE() << "Counting read guest memory"; });
        count.write(nullptr, 0);
        EXPECT_EQ(count.size(), huge + 7);
        EXPECT_THROW(count.get_buffer(), std::logic_error);
        EXPECT_THROW(count.write(nullptr, std::numeric_limits<size_t>::max()), std::length_error);
    }

    TEST(SnapshotStream, BreakOffsetsAndMovePreserveBufferedBehavior)
    {
        utils::buffer_serializer buffer{};
        buffer.set_break_offset(5);
        buffer.write<uint32_t>(0x12345678);
        EXPECT_THROW(buffer.write<uint8_t>(7), std::runtime_error);
        EXPECT_EQ(buffer.move_buffer(),
                  (std::vector<std::byte>{std::byte{4}, std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}}));
        EXPECT_EQ(buffer.size(), 0u);
        buffer.write<uint16_t>(6);
        EXPECT_EQ(buffer.size(), 3u);
    }

    TEST(SnapshotStream, ZstdFramesRetainKnownSizesAndLegacyDecompression)
    {
        for (const size_t size : {0u, 1u, 65537u, 1048579u})
        {
            std::vector<std::byte> original(size);
            uint32_t state = 0x1837;
            for (auto& value : original)
            {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                value = static_cast<std::byte>(state);
            }
            std::ostringstream output(std::ios::binary);
            utils::compression::zstd::stream_compressor compressor{output, size};
            for (size_t offset = 0; offset < size;)
            {
                const auto count = std::min<size_t>(10007, size - offset);
                compressor.write(std::span(original).subspan(offset, count));
                offset += count;
            }
            compressor.finish();
            const auto result = stream_bytes(output.str());
            EXPECT_EQ(ZSTD_getFrameContentSize(result.data(), result.size()), size);
            EXPECT_EQ(ZSTD_findFrameCompressedSize(result.data(), result.size()), result.size());
            EXPECT_EQ(utils::compression::zstd::decompress(result), original);
            compressor.finish();
            EXPECT_EQ(stream_bytes(output.str()), result);
            EXPECT_THROW(compressor.write({}), std::runtime_error);
        }
    }

    TEST(SnapshotStream, RefusesCountMismatchAndWriteFailures)
    {
        const std::array<std::byte, 3> data{};
        std::ostringstream output;
        utils::compression::zstd::stream_compressor short_input{output, 3};
        short_input.write(std::span(data).first(2));
        EXPECT_THROW(short_input.finish(), std::runtime_error);
        utils::compression::zstd::stream_compressor long_input{output, 2};
        EXPECT_THROW(long_input.write(data), std::runtime_error);
        rejecting_output rejected;
        std::ostream failed(&rejected);
        utils::compression::zstd::stream_compressor cannot_write{failed, 0};
        EXPECT_THROW(cannot_write.finish(), std::runtime_error);
    }

    TEST(SnapshotStream, SmallWritesCrossInputBufferBoundariesWithoutLosingTheTail)
    {
        const size_t count = 131089;
        std::vector<std::byte> expected(count);
        std::ostringstream output(std::ios::binary);
        utils::compression::zstd::stream_compressor compressor{output, count};
        for (size_t i = 0; i < count; ++i)
        {
            expected[i] = static_cast<std::byte>(i * 7);
            compressor.write(std::span(expected).subspan(i, 1));
        }
        compressor.finish();
        EXPECT_EQ(utils::compression::zstd::decompress(stream_bytes(output.str())), expected);
    }

    class SnapshotFile : public testing::Test
    {
      protected:
        std::filesystem::path directory;

        void SetUp() override
        {
            directory =
                std::filesystem::temp_directory_path() / ("sogen-snapshot-test-" + std::to_string(getpid()) + "-" +
                                                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            ASSERT_TRUE(std::filesystem::create_directory(directory));
        }

        void TearDown() override
        {
            std::error_code error{};
            std::filesystem::remove_all(directory, error);
        }

        static windows_emulator sample()
        {
            emulator_settings settings{};
            settings.disable_logging = true;
            settings.use_relative_time = true;
            settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }
    };

    TEST_F(SnapshotFile, StreamingSnapshotMatchesLegacyPayloadAndRestores)
    {
        auto emu = sample();
        emu.setup_process_if_necessary();
        const auto region = emu.memory.allocate_memory(0x31000, memory_permission::read_write);
        emu.emu().write_memory<uint64_t>(region + 0x10001, 0x12345678abcdef98ULL);
        const auto legacy = snapshot::create_emulator_snapshot(emu);
        const auto file = directory / "guest.snap";
        ASSERT_EQ(snapshot::write_emulator_snapshot(emu, file, false), file);
        const auto result = utils::io::read_file(file);
        ASSERT_GT(result.size(), 8u);
        EXPECT_TRUE(std::equal(result.begin(), result.begin() + 8, legacy.begin()));
        EXPECT_EQ(utils::compression::zstd::decompress(std::span(result).subspan(8)),
                  utils::compression::zstd::decompress(std::span(legacy).subspan(8)));
        auto restored = create_empty_emulator();
        snapshot::load_emulator_snapshot(restored, result);
        EXPECT_EQ(restored.memory.read_memory<uint64_t>(region + 0x10001), 0x12345678abcdef98ULL);
        EXPECT_EQ(restored.emu().read_instruction_pointer(), emu.emu().read_instruction_pointer());
        snapshot::write_emulator_snapshot(emu, file, false);
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}), 1);
    }

    TEST_F(SnapshotFile, RestoredHostDeadlinesFollowTheCurrentSteadyClockEpoch)
    {
        using steady_clock = std::chrono::steady_clock;
        const auto saved_now = steady_clock::time_point{std::chrono::seconds{1'278'655}};
        const auto restored_now = steady_clock::time_point{std::chrono::seconds{183'976}};
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        emulator_interfaces source_interfaces{};
        source_interfaces.clock = std::make_unique<snapshot_test_clock>(saved_now);
        auto source = create_sample_emulator(std::move(settings), {}, {}, std::move(source_interfaces));
        source.setup_process_if_necessary();
        const auto thread_handle = source.process.create_thread(source.memory, source.mod_manager.executable->entry_point, 0, 0x10000, 0);
        auto* thread = source.process.threads.get(thread_handle);
        ASSERT_NE(thread, nullptr);
        thread->await_time = saved_now + std::chrono::milliseconds{1};
        thread->await_io_completion = pending_io_completion_wait{};
        thread->await_io_completion->timeout = steady_clock::time_point::min();
        const user_timer_key key{.hwnd = 0x100, .timer_id = 7};
        thread->user_timers[key].due_time = saved_now + std::chrono::milliseconds{250};
        const auto saved_ticks = std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<1, 10'000'000>>>(
                                     saved_now.time_since_epoch())
                                     .count();
        source.process.kusd.access([&](KUSER_SHARED_DATA64& kusd) {
            kusd.InterruptTime.High1Time = static_cast<int32_t>(saved_ticks >> 32);
            kusd.InterruptTime.High2Time = kusd.InterruptTime.High1Time;
            kusd.InterruptTime.LowPart = static_cast<uint32_t>(saved_ticks);
        });

        utils::buffer_serializer serialized{};
        ASSERT_NO_THROW(source.serialize(serialized));

        emulator_settings restore_settings{.disable_logging = true};
        restore_settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        emulator_interfaces restore_interfaces{};
        restore_interfaces.clock = std::make_unique<snapshot_test_clock>(restored_now);
        auto restored = create_sample_emulator(std::move(restore_settings), {}, {}, std::move(restore_interfaces));
        utils::buffer_deserializer input{serialized.get_buffer()};
        {
            SCOPED_TRACE("new snapshot clock extension");
            ASSERT_NO_THROW(restored.deserialize(input));
        }

        const auto* resumed = restored.process.threads.get(thread_handle);
        ASSERT_NE(resumed, nullptr);
        EXPECT_EQ(resumed->await_time, restored_now + std::chrono::milliseconds{1});
        EXPECT_EQ(resumed->await_io_completion->timeout, steady_clock::time_point::min());
        EXPECT_EQ(resumed->user_timers.at(key).due_time, restored_now + std::chrono::milliseconds{250});
        EXPECT_EQ(input.get_remaining_size(), 0u);

        // Existing checkpoint files predate the explicit clock anchor and must remain resumable.
        auto legacy = serialized.get_buffer();
        // buffer_serializer prefixes each scalar with a one-byte length marker.
        constexpr auto extension_bytes = sizeof(uint64_t) + sizeof(steady_clock::duration::rep) + 2;
        ASSERT_GE(legacy.size(), extension_bytes);
        legacy.resize(legacy.size() - extension_bytes);
        emulator_settings legacy_settings{.disable_logging = true};
        legacy_settings.path_mappings["C:\\test-sample.exe"] = std::filesystem::current_path() / "test-sample.exe";
        emulator_interfaces legacy_interfaces{};
        legacy_interfaces.clock = std::make_unique<snapshot_test_clock>(restored_now);
        auto legacy_restored = create_sample_emulator(std::move(legacy_settings), {}, {}, std::move(legacy_interfaces));
        utils::buffer_deserializer legacy_input{legacy};
        {
            SCOPED_TRACE("legacy snapshot clock fallback");
            ASSERT_NO_THROW(legacy_restored.deserialize(legacy_input));
        }
        const auto* legacy_thread = legacy_restored.process.threads.get(thread_handle);
        ASSERT_NE(legacy_thread, nullptr);
        EXPECT_EQ(legacy_thread->await_time, restored_now + std::chrono::milliseconds{1});
        EXPECT_EQ(legacy_thread->user_timers.at(key).due_time, restored_now + std::chrono::milliseconds{250});
        EXPECT_EQ(legacy_input.get_remaining_size(), 0u);
    }

    TEST_F(SnapshotFile, FailedPublicationPreservesExistingDestinationAndCleansStaging)
    {
        auto emu = sample();
        emu.setup_process_if_necessary();
        const auto blocked = directory / "guest.snap";
        ASSERT_TRUE(std::filesystem::create_directory(blocked));
        const std::array<std::byte, 1> sentinel{std::byte{0xa5}};
        ASSERT_TRUE(utils::io::write_file(blocked / "sentinel", sentinel));
        EXPECT_THROW(snapshot::write_emulator_snapshot(emu, blocked, false), std::system_error);
        EXPECT_EQ(utils::io::read_file(blocked / "sentinel"), std::vector<std::byte>(sentinel.begin(), sentinel.end()));
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}), 1);
    }

    TEST_F(SnapshotFile, OpenGpuDeviceRejectsSaveBeforeReplacingDestination)
    {
        auto emu = sample();
        emu.setup_process_if_necessary();
        emu.process.devices.store(io_device_container{u"SogenGpu", emu, {}});
        const auto file = directory / "existing.snap";
        const std::array<std::byte, 1> sentinel{std::byte{0xa5}};
        ASSERT_TRUE(utils::io::write_file(file, sentinel));
        EXPECT_THROW(snapshot::write_emulator_snapshot(emu, file, false), std::runtime_error);
        EXPECT_EQ(utils::io::read_file(file), std::vector<std::byte>(sentinel.begin(), sentinel.end()));
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}), 1);
    }

    TEST(SnapshotStream, OpenGpuDeviceRejectsLegacyEmptyStateInBothGuestArchitectures)
    {
        for (const bool is_32_bit : {false, true})
        {
            SCOPED_TRACE(is_32_bit);
            auto device = create_device(u"SogenGpu", {.is_32_bit = is_32_bit});
            utils::buffer_deserializer old_state{std::span<const std::byte>{}};
            EXPECT_THROW(device->deserialize_object(old_state), std::runtime_error);
            auto count = utils::buffer_serializer::counting();
            EXPECT_THROW(device->serialize_object(count), std::runtime_error);
        }
    }

    TEST(SnapshotStream, FailureStopsAreNotSuccessfulCheckpointPauses)
    {
        for (const auto reason : {stop_reason::unknown_syscall, stop_reason::unimplemented_syscall, stop_reason::syscall_exception,
                                  stop_reason::unhandled_memory_violation, stop_reason::backend_error, stop_reason::signal_termination})
        {
            EXPECT_FALSE(snapshot::is_resumable_checkpoint_stop(reason));
        }
        for (const auto reason : {stop_reason::none, stop_reason::explicit_stop, stop_reason::breakpoint, stop_reason::watchpoint,
                                  stop_reason::instruction_limit})
        {
            EXPECT_TRUE(snapshot::is_resumable_checkpoint_stop(reason));
        }
    }

    TEST_F(SnapshotFile, ReportsCheckpointPauseSeparatelyFromHostSaveFailure)
    {
        std::string text;
        logger log;
        log.set_silent(true);
        log.set_sink([&](color, const std::string_view line) { text += line; });
        auto console = create_console_reporter(log, {});
        const auto file = directory / "report.jsonl";
        auto reporter = create_jsonl_reporter(file);
        run_finished_event paused{};
        paused.success = true;
        paused.checkpoint_saved = true;
        paused.rip = 0x12345;
        console->report(paused);
        reporter->report(paused);
        run_failed_event failure{};
        failure.rip = 0x12345;
        failure.message = "Disk write failed";
        failure.phase = "snapshot_save";
        console->report(failure);
        reporter->report(failure);
        reporter->flush();
        EXPECT_NE(text.find("Guest paused at: 0x12345 - checkpoint saved"), std::string::npos);
        EXPECT_NE(text.find("Snapshot save failed (host)"), std::string::npos);
        EXPECT_EQ(text.find("Emulation failed"), std::string::npos);
        const auto bytes = utils::io::read_file(file);
        const std::string json(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        EXPECT_NE(json.find("\"state\":\"paused\""), std::string::npos);
        EXPECT_NE(json.find("\"checkpoint_saved\":true"), std::string::npos);
        EXPECT_NE(json.find("\"phase\":\"snapshot_save\""), std::string::npos);
        EXPECT_EQ(json.find("\"exit\":"), std::string::npos);
    }

}
