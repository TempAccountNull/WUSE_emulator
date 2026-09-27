#include "../windows-emulator/package_read_trace.hpp"
#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

#include <gtest/gtest.h>

namespace sogen::test
{
    TEST(PackageReadTrace, DisabledTraceRecordsNothing)
    {
        package_read_trace trace(false);
        trace.record({.path = "ignored.pkg", .actual = 4});
        EXPECT_FALSE(trace.trigger_on_oodle("OODLE ERROR : LZ corruption"));
        EXPECT_EQ(trace.snapshot_json().find("ignored.pkg"), std::string::npos);
    }

    TEST(PackageReadTrace, RingFreezesOnFirstOodleAndRemainsBounded)
    {
        package_read_trace trace(true);
        for (size_t i = 0; i < package_read_trace::capacity + 3; ++i)
        {
            trace.record({.path = "read-" + std::to_string(i), .actual = i});
        }
        EXPECT_FALSE(trace.trigger_on_oodle("unrelated debug line"));
        EXPECT_TRUE(trace.trigger_on_oodle("OODLE ERROR : LZ corruption : bad decode len"));
        EXPECT_FALSE(trace.trigger_on_oodle("OODLE ERROR : another"));
        const auto frozen = trace.snapshot_json();
        EXPECT_EQ(frozen.find("read-0\""), std::string::npos);
        EXPECT_NE(frozen.find("read-3\""), std::string::npos);
        EXPECT_NE(frozen.find("read-34\""), std::string::npos);
        trace.record({.path = "too-late.pkg"});
        EXPECT_EQ(trace.snapshot_json(), frozen);
    }

    TEST(PackageReadTrace, IncrementalHashMatchesWholeBuffer)
    {
        constexpr std::string_view bytes = "a package payload";
        auto value = package_read_trace::hash_seed;
        value = package_read_trace::hash_append(value, std::span<const char>(bytes.data(), 5));
        value = package_read_trace::hash_append(value, std::span<const char>(bytes.data() + 5, bytes.size() - 5));
        EXPECT_EQ(value, package_read_trace::hash(std::span<const char>(bytes.data(), bytes.size())));
    }

    TEST(PackageReadTrace, ReportsWhenOnlyTheCappedPrefixWasHashed)
    {
        package_read_trace trace(true);
        trace.record({.path = "large.pkg", .actual = package_read_trace::max_hashed_bytes + 1,
                      .hashed_bytes = package_read_trace::max_hashed_bytes, .guest_valid = true});
        ASSERT_TRUE(trace.trigger_on_oodle("OODLE ERROR"));
        EXPECT_NE(trace.snapshot_json().find("\"truncated\":true"), std::string::npos);
    }

    TEST(PackageReadTrace, CapturesMismatchAndEscapesPath)
    {
        package_read_trace trace(true);
        constexpr std::string_view host = "package";
        constexpr std::string_view guest = "packXge";
        trace.record({
            .path = "d:\\packages\\bad\"name.pkg",
            .handle = 42,
            .tid = 48,
            .offset = 4096,
            .requested = 7,
            .actual = 7,
            .hashed_bytes = 7,
            .host_hash = package_read_trace::hash(std::span<const char>(host.data(), host.size())),
            .guest_hash = package_read_trace::hash(std::span<const char>(guest.data(), guest.size())),
            .guest_valid = true,
        });
        ASSERT_TRUE(trace.trigger_on_oodle("OODLE ERROR"));
        const auto json = trace.snapshot_json();
        EXPECT_NE(json.find("d:\\\\packages\\\\bad\\\"name.pkg"), std::string::npos);
        EXPECT_NE(json.find("\"offset\":4096"), std::string::npos);
        EXPECT_NE(json.find("\"hashed_bytes\":7"), std::string::npos);
        EXPECT_NE(json.find("\"guest_fnv1a64\":\""), std::string::npos);
        EXPECT_NE(package_read_trace::hash(std::span<const char>(host.data(), host.size())),
                  package_read_trace::hash(std::span<const char>(guest.data(), guest.size())));
    }
}

namespace sogen::syscalls
{
    NTSTATUS handle_NtReadFile(const syscall_context&, handle, uint64_t, uint64_t, uint64_t,
                               emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>, uint64_t, ULONG,
                               emulator_object<LARGE_INTEGER>, emulator_object<ULONG>);
    NTSTATUS handle_NtSetEvent(const syscall_context&, uint64_t, emulator_object<LONG>);
}

namespace sogen::test
{
    TEST(PackageReadTraceIntegration, NtReadFileThenDbwinOodleFreezesActualRead)
    {
        const auto* previous_value = std::getenv("SOGEN_TRACE_PACKAGE_READS");
        const auto previous = previous_value ? std::string(previous_value) : std::string{};
        ASSERT_EQ(_putenv_s("SOGEN_TRACE_PACKAGE_READS", "1"), 0);
        emulator_settings settings{.disable_logging = true};
        settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
        auto emu = create_sample_emulator(std::move(settings));
        ASSERT_EQ(_putenv_s("SOGEN_TRACE_PACKAGE_READS", previous.c_str()), 0);
        ASSERT_TRUE(emu.package_reads_trace.enabled());
        emu.setup_process_if_necessary();

        FILE* native = std::tmpfile();
        ASSERT_NE(native, nullptr);
        constexpr char contents[] = "ABCDEFGHIJ";
        ASSERT_EQ(std::fwrite(contents, 1, sizeof(contents) - 1, native), sizeof(contents) - 1);
        std::rewind(native);
        file target{};
        target.name = u"\\??\\d:\\packages\\trace.pkg";
        target.handle = native;
        const auto handle = emu.process.files.store(std::move(target));

        const auto memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        const LARGE_INTEGER offset{.QuadPart = 2};
        emu.memory.write_memory(memory + 0x40, &offset, sizeof(offset));
        auto& vcpu = emu.vcpu(0);
        const syscall_context context{.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        const emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>> io_status{emu.memory, memory};
        ASSERT_EQ(syscalls::handle_NtReadFile(context, handle, 0, 0, 0, io_status, memory + 0x100, 4,
                                              {emu.memory, memory + 0x40}, {emu.memory}),
                  STATUS_SUCCESS);
        EXPECT_EQ(io_status.read().Information, 4u);
        std::array<char, 4> guest{};
        ASSERT_TRUE(emu.memory.try_read_memory(memory + 0x100, guest.data(), guest.size()));
        EXPECT_EQ(std::string_view(guest.data(), guest.size()), "CDEF");

        const auto dbwin = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
        emu.process.dbwin_buffer = dbwin;
        constexpr char error[] = "OODLE ERROR : LZ corruption : bad decode len";
        emu.memory.write_memory(dbwin + 4, error, sizeof(error));
        std::vector<std::string> messages;
        emu.callbacks.on_debug_string.add([&](const std::string_view message) { messages.emplace_back(message); });
        ASSERT_EQ(syscalls::handle_NtSetEvent(context, DBWIN_DATA_READY.bits, {emu.memory, 0}), STATUS_SUCCESS);
        ASSERT_EQ(messages.size(), 2u);
        EXPECT_EQ(messages[0], error);
        EXPECT_NE(messages[1].find("Package read snapshot at first OODLE ERROR:"), std::string::npos);
        const auto snapshot = emu.package_reads_trace.snapshot_json();
        EXPECT_NE(snapshot.find("trace.pkg"), std::string::npos);
        EXPECT_NE(snapshot.find("\"offset\":2"), std::string::npos);
        EXPECT_NE(snapshot.find("\"requested\":4"), std::string::npos);
        EXPECT_NE(snapshot.find("\"actual\":4"), std::string::npos);
        EXPECT_NE(snapshot.find("\"hashed_bytes\":4"), std::string::npos);
        EXPECT_NE(snapshot.find("\"truncated\":false"), std::string::npos);
        EXPECT_FALSE(emu.package_reads_trace.trigger_on_oodle(error));
        EXPECT_EQ(emu.package_reads_trace.snapshot_json(), snapshot);
    }
}