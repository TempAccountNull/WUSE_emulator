#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <utils/finally.hpp>
#include <memory_manager.hpp>

#include <gtest/gtest.h>
#include <cstdint>

namespace sogen::test
{
    TEST(IcicleSmp, ReuseAddressOnPeerBeforeQueuedUnmapDrains)
    {
        constexpr uint64_t address = 0x44000000;
        constexpr uint64_t earlier_map = 0x45000000;
        constexpr size_t page_size = 0x1000;
        constexpr uint64_t old_value = 0x1122334455667788;
        constexpr uint64_t new_value = 0x8877665544332211;

        auto emu = icicle::create_x86_64_emulator(2);
        ASSERT_EQ(emu->vcpu_count(), 2U);
        memory_manager memory(*emu);
        ASSERT_TRUE(memory.allocate_memory(address, page_size, memory_permission::read_write));
        ASSERT_NO_THROW(emu->write_memory(address, &old_value, sizeof(old_value)));

        const auto read_on_worker = [&](const size_t index, uint64_t& value) {
            emu->set_scheduler_worker_context(index, true);
            const auto clear = utils::finally([&] { emu->set_scheduler_worker_context(index, false); });
            return emu->try_read_memory(address, &value, sizeof(value));
        };

        uint64_t observed{};
        ASSERT_TRUE(read_on_worker(1, observed));
        ASSERT_EQ(observed, old_value);

        const auto before_unmap = emu->smp_op_watermark();
        emu->set_scheduler_worker_context(0, true);
        {
            const auto clear = utils::finally([&] { emu->set_scheduler_worker_context(0, false); });
            ASSERT_TRUE(memory.allocate_memory(earlier_map, page_size, memory_permission::read_write));
            ASSERT_TRUE(memory.release_memory(address, 0));
        }
        const auto unmap_mark = emu->smp_op_watermark();
        ASSERT_GT(unmap_mark, before_unmap);
        ASSERT_FALSE(emu->smp_op_applied(unmap_mark));
        ASSERT_TRUE(read_on_worker(1, observed));
        ASSERT_EQ(observed, old_value);

        bool remapped = false;
        emu->set_scheduler_worker_context(1, true);
        {
            const auto clear = utils::finally([&] { emu->set_scheduler_worker_context(1, false); });
            ASSERT_NO_THROW(remapped = memory.allocate_memory(address, page_size, memory_permission::read_write));
        }
        ASSERT_TRUE(remapped);
        ASSERT_TRUE(emu->smp_op_applied(unmap_mark));
        emu->set_scheduler_worker_context(1, true);
        {
            const auto clear = utils::finally([&] { emu->set_scheduler_worker_context(1, false); });
            ASSERT_TRUE(emu->try_read_memory(earlier_map, &observed, sizeof(observed)));
            EXPECT_EQ(observed, 0U);
        }

        ASSERT_NO_THROW(emu->sync_worker_context(0));
        ASSERT_NO_THROW(emu->sync_worker_context(1));
        ASSERT_TRUE(emu->smp_op_applied(emu->smp_op_watermark()));
        ASSERT_TRUE(read_on_worker(0, observed));
        EXPECT_EQ(observed, 0U);
        ASSERT_TRUE(read_on_worker(1, observed));
        EXPECT_EQ(observed, 0U);

        ASSERT_NO_THROW(emu->write_memory(address, &new_value, sizeof(new_value)));
        ASSERT_TRUE(read_on_worker(0, observed));
        EXPECT_EQ(observed, new_value);
        ASSERT_TRUE(read_on_worker(1, observed));
        EXPECT_EQ(observed, new_value);
    }
}
