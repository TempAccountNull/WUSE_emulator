#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <memory_manager.hpp>
#include <utils/finally.hpp>

#include <gtest/gtest.h>
#include <array>
#include <cstdlib>
#include <string>

namespace sogen::test
{
    namespace
    {
        enum class queue_barrier
        {
            none,
            map,
            protect,
        };

        void check_peer_invalidation_queue(const queue_barrier barrier)
        {
            const char* previous = std::getenv("SOGEN_SMP_PROFILE");
            const std::string old_value = previous ? previous : "";
            ASSERT_EQ(_putenv_s("SOGEN_SMP_PROFILE", "1"), 0);
            const auto restore = utils::finally([&] { _putenv_s("SOGEN_SMP_PROFILE", old_value.c_str()); });

            auto emu = icicle::create_x86_64_emulator(2);
            ASSERT_EQ(emu->vcpu_count(), 2U);
            memory_manager memory(*emu);
            const auto code = memory.allocate_memory(0x1000, memory_permission::all);
            ASSERT_NE(code, 0U);

            // mov eax,imm32; jmp back to the mov. Both VMs cache v1 before any mutation.
            constexpr std::array<uint8_t, 7> v1{0xB8, 0x11, 0x11, 0, 0, 0xEB, 0xF9};
            constexpr std::array<uint8_t, 7> v2{0xB8, 0x22, 0x22, 0, 0, 0xEB, 0xF9};
            constexpr std::array<uint8_t, 7> v3{0xB8, 0x33, 0x33, 0, 0, 0xEB, 0xF9};
            emu->write_memory(code, v1.data(), v1.size());
            auto& cpu0 = emu->get_cpu(0);
            auto& cpu1 = emu->get_cpu(1);
            cpu0.reg(x86_register::rip, code);
            cpu0.start(10);
            ASSERT_EQ(cpu0.reg(x86_register::rax), 0x1111U);
            cpu1.reg(x86_register::rip, code);
            cpu1.start(10);
            ASSERT_EQ(cpu1.reg(x86_register::rax), 0x1111U);

            const auto before = emu->smp_profile();
            ASSERT_EQ(before.size(), 2U);
            const auto ticket_before = emu->smp_op_watermark();

            emu->set_scheduler_worker_context(0, true);
            {
                const auto clear = utils::finally([&] { emu->set_scheduler_worker_context(0, false); });
                emu->write_memory(code, v2.data(), v2.size());
                if (barrier == queue_barrier::map)
                {
                    ASSERT_TRUE(memory.allocate_memory(0x3000, 0x1000, memory_permission::read_write));
                }
                if (barrier == queue_barrier::protect)
                {
                    // Same permissions still enqueue a peer protection refresh between invalidations.
                    ASSERT_TRUE(memory.protect_memory(code, 0x1000, memory_permission::all));
                }
                cpu0.reg(x86_register::rip, code);
                cpu0.start(10); // Recache on the writer so its next write queues another invalidation.
                ASSERT_EQ(cpu0.reg(x86_register::rax), 0x2222U);
                emu->write_memory(code, v3.data(), v3.size());
            }

            const auto ticket_after = emu->smp_op_watermark();
            ASSERT_GT(ticket_after, ticket_before);
            EXPECT_FALSE(emu->smp_op_applied(ticket_after));
            const auto queued = emu->smp_profile();
            ASSERT_EQ(queued.size(), 2U);
            EXPECT_EQ(queued[1].invalidate_queued - before[1].invalidate_queued, 2U);
            EXPECT_EQ(queued[1].invalidate_adjacent_same_pages - before[1].invalidate_adjacent_same_pages,
                      barrier == queue_barrier::none ? 1U : 0U);

            ASSERT_NO_THROW(emu->sync_worker_context(1));
            EXPECT_TRUE(emu->smp_op_applied(ticket_after));
            const auto applied = emu->smp_profile();
            ASSERT_EQ(applied.size(), 2U);
            EXPECT_EQ(applied[1].invalidate_applied - before[1].invalidate_applied, 2U);
            if (barrier == queue_barrier::none)
            {
                EXPECT_GE(applied[1].invalidate_no_change - before[1].invalidate_no_change, 1U);
            }

            cpu1.reg(x86_register::rip, code);
            cpu1.start(10);
            EXPECT_EQ(cpu1.reg(x86_register::rax), 0x3333U);
        }
    }

    TEST(IcicleSmp, ConsecutivePeerInvalidationsAreCountedWithoutCoalescing)
    {
        check_peer_invalidation_queue(queue_barrier::none);
    }

    TEST(IcicleSmp, PeerMapBarrierPreservesInvalidationFifo)
    {
        check_peer_invalidation_queue(queue_barrier::map);
    }

    TEST(IcicleSmp, PeerProtectionBarrierPreservesInvalidationFifo)
    {
        check_peer_invalidation_queue(queue_barrier::protect);
    }

    TEST(IcicleSmp, ExternalWriterInvalidationsAreCountedForBothTargets)
    {
        const char* previous = std::getenv("SOGEN_SMP_PROFILE");
        const std::string old_value = previous ? previous : "";
        ASSERT_EQ(_putenv_s("SOGEN_SMP_PROFILE", "1"), 0);
        const auto restore = utils::finally([&] { _putenv_s("SOGEN_SMP_PROFILE", old_value.c_str()); });

        auto emu = icicle::create_x86_64_emulator(2);
        memory_manager memory(*emu);
        const auto code = memory.allocate_memory(0x1000, memory_permission::all);
        ASSERT_NE(code, 0U);
        constexpr std::array<uint8_t, 7> v1{0xB8, 0x11, 0x11, 0, 0, 0xEB, 0xF9};
        constexpr std::array<uint8_t, 7> v2{0xB8, 0x22, 0x22, 0, 0, 0xEB, 0xF9};
        emu->write_memory(code, v1.data(), v1.size());
        for (size_t i = 0; i < 2; ++i)
        {
            auto& cpu = emu->get_cpu(i);
            cpu.reg(x86_register::rip, code);
            cpu.start(10);
            ASSERT_EQ(cpu.reg(x86_register::rax), 0x1111U);
        }
        const auto before = emu->smp_profile();
        ASSERT_EQ(before.size(), 2U);

        // No scheduler-worker context: this is the external loader/write path.
        emu->write_memory(code, v2.data(), v2.size());
        const auto queued = emu->smp_profile();
        ASSERT_EQ(queued.size(), 2U);
        for (size_t i = 0; i < 2; ++i)
        {
            EXPECT_EQ(queued[i].invalidate_queued - before[i].invalidate_queued, 1U);
            ASSERT_NO_THROW(emu->sync_worker_context(i));
            auto& cpu = emu->get_cpu(i);
            cpu.reg(x86_register::rip, code);
            cpu.start(10);
            EXPECT_EQ(cpu.reg(x86_register::rax), 0x2222U);
        }
    }
}
