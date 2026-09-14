#include "emulation_test_utils.hpp"
#include <devices/network_store_interface.hpp>

namespace sogen::test
{
    TEST(NsiLayoutTest, AllParameterBuffersHaveIndependentOffsetsAndBounds)
    {
        auto emu = create_empty_emulator();
        auto device = create_network_store_interface({});
        const auto allocation = emu.memory.allocate_memory(0x2000, memory_permission::read_write);
        emu.emu().set_memory(allocation, 0xa5, 0x2000);
        const uint64_t key_address = allocation + 0x100;
        const uint64_t rw_address = allocation + 0x200;
        const uint64_t dynamic_address = allocation + 0x800;
        const uint64_t static_address = allocation + 0xc00;
        std::array<uint64_t, 13> request{};
        request[5] = key_address;
        request[6] = 8;
        request[7] = rw_address;
        request[8] = 16;
        request[9] = dynamic_address;
        request[10] = 0x290;
        request[11] = static_address;
        request[12] = 0x238;
        emu.memory.write_memory(allocation, request.data(), sizeof(request));
        io_device_context context{emu.memory};
        context.input_buffer = allocation;
        context.input_buffer_length = sizeof(request);
        context.output_buffer = allocation;
        context.output_buffer_length = sizeof(request);
        context.io_control_code = 0x12000f;
        ASSERT_EQ(device->io_control(emu, context), STATUS_SUCCESS);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(rw_address), 0);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(rw_address + 16), 0xa5);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(dynamic_address), 0);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(dynamic_address + 0x28f), 0);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(dynamic_address + 0x290), 0xa5);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(static_address + 0x238), 0xa5);
        request[7] = 0;
        request[8] = 0;
        emu.memory.write_memory(allocation, request.data(), sizeof(request));
        ASSERT_EQ(device->io_control(emu, context), STATUS_SUCCESS);
    }
}
