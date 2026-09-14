#include "emulation_test_utils.hpp"
#include <devices/network_store_interface.hpp>

namespace sogen::test
{
    namespace
    {
        struct query_socket_factory : network::socket_factory
        {
            std::function<uint32_t(network::network_store_query&)> query;
            uint32_t calls{};

            uint32_t query_network_store(network::network_store_query& request) override
            {
                ++calls;
                return query(request);
            }
        };

        struct NsiLayoutTest : testing::Test
        {
            query_socket_factory* backend{};
            windows_emulator emu;
            std::unique_ptr<io_device> device = create_network_store_interface({});
            uint64_t allocation{};
            io_device_context context;

            NsiLayoutTest()
                : emu(make_emulator()),
                  context(emu.memory)
            {
                allocation = emu.memory.allocate_memory(0x6000, memory_permission::read_write);
                emu.emu().set_memory(allocation, 0xa5, 0x6000);
                const std::array<uint32_t, 6> module = {24, 1, 0xeb004a11, 0x11d49b1a, 0x50002391, 0xbc597704};
                emu.memory.write_memory(allocation + 0x100, module.data(), sizeof(module));
                context.input_buffer = allocation;
                context.output_buffer = allocation;
                context.io_status_block = {emu.memory, allocation + 0x200};
            }

            windows_emulator make_emulator()
            {
                auto factory = std::make_unique<query_socket_factory>();
                backend = factory.get();
                return create_empty_emulator({.socket_factory = std::move(factory)});
            }

            void all_request()
            {
                const std::array<uint64_t, 13> request = {
                    0, 0, allocation + 0x100, 1, 1, allocation + 0x300, 8, 0, 0, allocation + 0x1000, 0x290, allocation + 0x2000, 0x238};
                emu.memory.write_memory(allocation, request.data(), sizeof(request));
                context.input_buffer_length = sizeof(request);
                context.output_buffer_length = sizeof(request);
                context.io_control_code = 0x12000f;
            }
        };
    }

    TEST_F(NsiLayoutTest, NativeInterfaceQueryPreservesKeyAndReturnsDynamicAndStaticData)
    {
        all_request();
        backend->query = [&](network::network_store_query& query) {
            EXPECT_EQ(query.operation, network::network_store_operation::all_parameters);
            EXPECT_EQ(query.table, 1U);
            EXPECT_EQ(query.flags, 1U);
            EXPECT_EQ(query.buffers[0].bytes, std::vector<uint8_t>(8, 0xa5));
            EXPECT_TRUE(query.buffers[1].bytes.empty());
            EXPECT_EQ(query.buffers[2].bytes.size(), 0x290U);
            EXPECT_EQ(query.buffers[3].bytes.size(), 0x238U);
            query.buffers[0].bytes[0] = 0xff;
            query.buffers[2].bytes[536] = 1;
            query.buffers[3].bytes[0] = 3;
            return 0U;
        };
        ASSERT_EQ(device->io_control(emu, context), STATUS_SUCCESS);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x300), 0xa5);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x1000 + 536), 1);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x1290), 0xa5);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x2000), 3);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x2238), 0xa5);
        EXPECT_EQ(context.io_status_block.read().Information, 0U);
    }

    TEST_F(NsiLayoutTest, ParameterUsesNestedOutputAndFourByteLengthDespitePadding)
    {
        const std::array<uint64_t, 10> request = {
            0, 0, allocation + 0x100, 0, 1, allocation + 0x300, 32, 1, allocation + 0x1000, (uint64_t{88} << 32) | 4};
        emu.memory.write_memory(allocation, request.data(), sizeof(request));
        context.input_buffer_length = sizeof(request);
        context.output_buffer_length = 4;
        context.io_control_code = 0x120007;
        backend->query = [](network::network_store_query& query) {
            EXPECT_EQ(query.parameter_type, 1U);
            EXPECT_EQ(query.parameter_offset, 88U);
            EXPECT_EQ(query.buffers[0].bytes.size(), 32U);
            EXPECT_EQ(query.buffers[1].bytes.size(), 4U);
            query.buffers[1].bytes = {3, 0, 0, 0};
            return 0U;
        };
        ASSERT_EQ(device->io_control(emu, context), STATUS_SUCCESS);
        EXPECT_EQ(emu.emu().read_memory<uint32_t>(allocation + 0x1000), 3U);
        EXPECT_EQ(emu.emu().read_memory<uint64_t>(allocation), 0U);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x1004), 0xa5);
    }

    TEST_F(NsiLayoutTest, EnumerationHonorsCapacityAndReportsRequiredCount)
    {
        std::array<uint64_t, 14> request = {0,
                                            0,
                                            allocation + 0x100,
                                            1,
                                            (uint64_t{1} << 32) | 1,
                                            allocation + 0x300,
                                            8,
                                            0,
                                            0,
                                            allocation + 0x1000,
                                            0x290,
                                            allocation + 0x2000,
                                            0x238,
                                            0};
        context.input_buffer_length = sizeof(request);
        context.output_buffer_length = sizeof(request);
        context.io_control_code = 0x12001b;
        backend->query = [](network::network_store_query& query) {
            EXPECT_EQ(query.second_flags, 1U);
            EXPECT_EQ(query.buffers[0].bytes.size(), query.count * 8U);
            EXPECT_EQ(query.buffers[2].bytes.size(), query.count * 0x290U);
            if (query.count)
            {
                query.buffers[0].bytes[0] = 3;
                query.buffers[2].bytes[0] = 7;
                query.buffers[3].bytes[0] = 9;
            }
            query.count = 2;
            return 234U;
        };
        for (uint64_t capacity : {0, 1})
        {
            request[13] = capacity;
            emu.memory.write_memory(allocation, request.data(), sizeof(request));
            ASSERT_EQ(device->io_control(emu, context), STATUS_MORE_ENTRIES);
            EXPECT_EQ(emu.emu().read_memory<uint32_t>(allocation + 0x68), 2U);
            EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x308), 0xa5);
            EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x1290), 0xa5);
        }
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x300), 3);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x1000), 7);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x2000), 9);
    }

    TEST_F(NsiLayoutTest, Wow64InterfaceQueryUsesFourByteDescriptors)
    {
        constexpr uint32_t base = 0x10000000;
        ASSERT_TRUE(emu.memory.allocate_memory(base, 0x4000, memory_permission::read_write));
        emu.emu().set_memory(base, 0xa5, 0x4000);
        const std::array<uint32_t, 6> module = {24, 1, 0xeb004a11, 0x11d49b1a, 0x50002391, 0xbc597704};
        emu.memory.write_memory(base + 0x100, module.data(), sizeof(module));
        const std::array<uint32_t, 14> request = {
            0, 0, base + 0x100, 1, 1, 0, base + 0x300, 8, 0, 0, base + 0x1000, 0x290, base + 0x2000, 0x238};
        emu.memory.write_memory(base, request.data(), sizeof(request));
        emu.process.is_wow64_process = true;
        context.input_buffer = base;
        context.output_buffer = base;
        context.input_buffer_length = sizeof(request);
        context.output_buffer_length = sizeof(request);
        context.io_control_code = 0x12000f;
        backend->query = [](network::network_store_query& query) {
            EXPECT_EQ(query.table, 1U);
            EXPECT_EQ(query.flags, 1U);
            EXPECT_EQ(query.buffers[0].bytes.size(), 8U);
            EXPECT_TRUE(query.buffers[1].bytes.empty());
            EXPECT_EQ(query.buffers[2].bytes.size(), 0x290U);
            EXPECT_EQ(query.buffers[3].bytes.size(), 0x238U);
            query.buffers[2].bytes[536] = 1;
            query.buffers[3].bytes[0] = 3;
            return 0U;
        };
        ASSERT_EQ(device->io_control(emu, context), STATUS_SUCCESS);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(base + 0x300), 0xa5);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(base + 0x1000 + 536), 1);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(base + 0x1290), 0xa5);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(base + 0x2000), 3);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(base + 0x2238), 0xa5);
    }

    TEST_F(NsiLayoutTest, QueryFailureDoesNotOverwriteGuestOutputs)
    {
        all_request();
        backend->query = [](network::network_store_query& query) {
            query.buffers[2].bytes[0] = 0;
            return 1168U;
        };
        EXPECT_EQ(device->io_control(emu, context), STATUS_NOT_FOUND);
        EXPECT_EQ(emu.emu().read_memory<uint8_t>(allocation + 0x1000), 0xa5);
        EXPECT_EQ(context.io_status_block.read().Status, STATUS_NOT_FOUND);
    }

    TEST_F(NsiLayoutTest, InvalidAndOversizedRequestsDoNotReachBackend)
    {
        all_request();
        context.input_buffer_length = 0x60;
        EXPECT_EQ(device->io_control(emu, context), STATUS_INVALID_PARAMETER);
        context.input_buffer_length = 0x68;
        emu.emu().write_memory<uint32_t>(allocation + 0x50, 0xffffffff);
        EXPECT_EQ(device->io_control(emu, context), STATUS_INVALID_PARAMETER);
        all_request();
        emu.emu().write_memory<uint64_t>(allocation + 0x48, 0);
        EXPECT_EQ(device->io_control(emu, context), STATUS_INVALID_PARAMETER);
        context.io_control_code = 0x120013;
        EXPECT_EQ(device->io_control(emu, context), STATUS_NOT_SUPPORTED);
        EXPECT_EQ(backend->calls, 0U);
    }
}
