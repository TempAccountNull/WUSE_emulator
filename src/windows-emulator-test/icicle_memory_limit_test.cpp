#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <utils/finally.hpp>
#include <cstdlib>
#include <string>
#include <array>

#ifdef OS_WINDOWS
namespace sogen::test
{
    TEST(IcicleMemoryLimit, GuestStoreReportsConfiguredLimitAndFaultAddress)
    {
        const auto* previous = std::getenv("SOGEN_ICICLE_MEMORY_MB");
        const std::string saved = previous ? previous : "";
        ASSERT_EQ(_putenv_s("SOGEN_ICICLE_MEMORY_MB", "1"), 0);
        const auto restore = utils::finally([&] { _putenv_s("SOGEN_ICICLE_MEMORY_MB", saved.c_str()); });
        auto emu = create_x86_64_emulator(backend_type::icicle);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(4096, memory_permission::all);
        const auto data = memory.allocate_memory(2 * 1024 * 1024, memory_permission::read_write);
        ASSERT_NE(code, 0u);
        ASSERT_NE(data, 0u);
        const std::array<uint8_t, 10> bytes{0x89, 0x00, 0x48, 0x05, 0x00, 0x10, 0x00, 0x00, 0xEB, 0xF6};
        emu->write_memory(code, bytes.data(), bytes.size());
        emu->reg(x86_register::rip, code);
        emu->reg(x86_register::rax, data);
        try
        {
            emu->start(2048);
            FAIL() << "Guest store unexpectedly exceeded the configured physical-page limit";
        }
        catch (const std::runtime_error& error)
        {
            const std::string message = error.what();
            EXPECT_NE(message.find("OutOfMemory; physical_pages=256/256;"), std::string::npos) << message;
            EXPECT_NE(message.find("code=0x"), std::string::npos) << message;
            EXPECT_NE(message.find("value=0x"), std::string::npos) << message;
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), code);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), data + 253 * 4096);
        }
    }

    TEST(IcicleMemoryLimit, InvalidEnvironmentValuesAreRejectedBeforeExecution)
    {
        const auto* previous = std::getenv("SOGEN_ICICLE_MEMORY_MB");
        const std::string saved = previous ? previous : "";
        const auto restore = utils::finally([&] { _putenv_s("SOGEN_ICICLE_MEMORY_MB", saved.c_str()); });
        for (const auto* invalid : {"0", "-1", "abc", "1MiB", " 1", "1 ", "16777216", "18446744073709551616"})
        {
            SCOPED_TRACE(invalid);
            ASSERT_EQ(_putenv_s("SOGEN_ICICLE_MEMORY_MB", invalid), 0);
            EXPECT_THROW(create_x86_64_emulator(backend_type::icicle), std::runtime_error);
        }
    }
}
#endif
