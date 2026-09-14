#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>
#include <bit>

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiInit(const syscall_context&);
    NTSTATUS handle_NtGdiInit2(const syscall_context&);
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context&, hdc);
    uint64_t handle_NtGdiCreateSolidBrush(const syscall_context&, uint32_t, uint64_t);
    uint32_t handle_NtGdiDeleteObjectApp(const syscall_context&, uint32_t);
}

namespace sogen::test
{
    class GdiPointerTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        mapped_module win32u{};
        mapped_module* original{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            original = emu.mod_manager.win32u;
            win32u.name = "win32u.dll";
            emu.mod_manager.win32u = &win32u;
        }

        void TearDown() override
        {
            emu.mod_manager.win32u = original;
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        void use_cookie_abi(const uint32_t cookie)
        {
            win32u.exports.push_back({.name = "NtGdiInit2", .address = 0x1234});
            emu.process.peb64.access([&](PEB64& peb) { peb.GdiDCAttributeList = cookie; });
        }

        GDI_HANDLE_ENTRY64 entry(const uint64_t handle) const
        {
            const auto table = emu.process.peb64.read().GdiSharedHandleTable;
            return emu.memory.read_memory<GDI_HANDLE_ENTRY64>(table + (handle & 0xFFFF) * sizeof(GDI_HANDLE_ENTRY64));
        }

        uint64_t create_dc()
        {
            return syscalls::handle_NtGdiCreateCompatibleDC(context(), 0);
        }
    };

    TEST_F(GdiPointerTest, LegacyInitReturnsTrueAndPreservesBatchLimit)
    {
        emu.process.peb64.access([](PEB64& peb) { peb.GdiDCAttributeList = 0x123; });
        EXPECT_EQ(syscalls::handle_NtGdiInit(context()), TRUE);
        EXPECT_EQ(emu.process.peb64.read().GdiDCAttributeList, 0x123u);
        const auto dc = create_dc();
        ASSERT_NE(dc, 0u);
        const auto item = entry(dc);
        EXPECT_EQ(item.UserPointer, item.Object);
        EXPECT_NE(item.UserPointer, 0u);
        EXPECT_NO_THROW((emu.memory.read_memory<std::array<uint8_t, 0x130>>(item.UserPointer)));
    }

    TEST_F(GdiPointerTest, LegacyBrushAndDcHaveRawUserPointers)
    {
        const auto dc = create_dc();
        const auto brush = syscalls::handle_NtGdiCreateSolidBrush(context(), 0x123456, 0);
        ASSERT_NE(dc, 0u);
        ASSERT_NE(brush, 0u);
        EXPECT_EQ(entry(dc).UserPointer, entry(dc).Object);
        EXPECT_EQ(entry(brush).UserPointer, entry(brush).Object);
    }

    TEST_F(GdiPointerTest, LegacyCookieFromOldSnapshotDoesNotEncodeNewObjects)
    {
        emu.process.peb64.access([](PEB64& peb) { peb.GdiDCAttributeList = 1; });
        const auto dc = create_dc();
        const auto item = entry(dc);
        ASSERT_NE(item.Object, 0u);
        EXPECT_EQ(item.UserPointer, item.Object);
    }

    TEST_F(GdiPointerTest, CookieAbiReturnsCookieAndEncodesPointers)
    {
        constexpr uint32_t cookie = 0x1235;
        use_cookie_abi(cookie);
        EXPECT_EQ(syscalls::handle_NtGdiInit(context()), TRUE);
        EXPECT_EQ(syscalls::handle_NtGdiInit2(context()), static_cast<NTSTATUS>(cookie));
        const auto dc = create_dc();
        const auto item = entry(dc);
        EXPECT_NE(item.UserPointer, item.Object);
        EXPECT_EQ(std::rotr(item.UserPointer, static_cast<int>(64 - (cookie & 63))) ^ cookie, item.Object);
    }

    TEST_F(GdiPointerTest, CookieAbiInitializesStableNonzeroCookie)
    {
        use_cookie_abi(0);
        const auto cookie = static_cast<uint32_t>(syscalls::handle_NtGdiInit2(context()));
        ASSERT_NE(cookie, 0u);
        EXPECT_EQ(static_cast<uint32_t>(syscalls::handle_NtGdiInit2(context())), cookie);
        const auto item = entry(create_dc());
        EXPECT_EQ(std::rotr(item.UserPointer, static_cast<int>(64 - (cookie & 63))) ^ cookie, item.Object);
    }

    TEST_F(GdiPointerTest, Wow64CookieAbiUsesThirtyTwoBitRotation)
    {
        constexpr uint32_t cookie = 0xABCDEF07;
        use_cookie_abi(cookie);
        emu.process.is_wow64_process = true;
        const auto item = entry(create_dc());
        EXPECT_LE(item.UserPointer, std::numeric_limits<uint32_t>::max());
        EXPECT_EQ(std::rotr(static_cast<uint32_t>(item.UserPointer), static_cast<int>(32 - (cookie & 31))) ^ cookie, item.Object);
        emu.process.is_wow64_process = false;
    }

    TEST_F(GdiPointerTest, DeletedLegacyHandleReuseKeepsRawPointerAndChangesGeneration)
    {
        const auto first = create_dc();
        ASSERT_EQ(syscalls::handle_NtGdiDeleteObjectApp(context(), static_cast<uint32_t>(first)), TRUE);
        const auto next = create_dc();
        EXPECT_EQ(first & 0xFFFF, next & 0xFFFF);
        EXPECT_NE(first, next);
        EXPECT_EQ(entry(next).UserPointer, entry(next).Object);
    }
}
