#include "../backends/icicle-emulator/icicle_x86_64_emulator.hpp"
#include <utils/finally.hpp>
#include <memory_manager.hpp>

#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace sogen::test
{
    // Run in separate processes with SOGEN_ICICLE_SMP_BATCH_UNMAP=0 and =1:
    // the Rust MMU caches the flag once. Exercise the public C++ fanout too.
    TEST(IcicleSmp, SharedMiddleUnmapAndRemapPreservesPeerCoherence)
    {
        constexpr uint64_t page = 0x1000;
        constexpr uint64_t data = 0x40000000;
        constexpr uint64_t code = 0x50000000;
        constexpr size_t middle_pages = 256;
        constexpr size_t middle_len = middle_pages * page;
        constexpr size_t total_len = middle_len + 2 * page;
        constexpr uint64_t middle = data + page;
        constexpr uint64_t right = middle + middle_len;
        constexpr uint64_t left_value = 0x1122334455667788ULL;
        constexpr uint64_t old_value = 0x8877665544332211ULL;
        constexpr uint64_t right_value = 0x1020304050607080ULL;
        constexpr uint64_t replacement = 0xdec0dedec0dedec0ULL;

        auto emu = icicle::create_x86_64_emulator(2);
        ASSERT_EQ(emu->vcpu_count(), 2U);
        memory_manager memory(*emu);
        ASSERT_TRUE(memory.allocate_memory(data, total_len, memory_permission::read_write));
        ASSERT_TRUE(memory.allocate_memory(code, page, memory_permission::all));

        // Fixed instructions keep guest translations stable across unmap/remap.
        std::array<uint8_t, 0x100> program{};
        program.fill(0x90);
        const auto movabs = [&](const size_t offset, const uint8_t opcode, const uint64_t immediate) {
            program[offset] = 0x48;
            program[offset + 1] = opcode;
            std::memcpy(program.data() + offset + 2, &immediate, sizeof(immediate));
        };
        const auto load = [&](const size_t offset, const uint64_t address) {
            movabs(offset, 0xB8, address); // mov rax, address
            program[offset + 10] = 0x48;
            program[offset + 11] = 0x8B;
            program[offset + 12] = 0x00; // mov rax, [rax]
        };
        load(0x00, data);
        load(0x20, middle);
        load(0x40, right);
        movabs(0x80, 0xB8, middle);
        movabs(0x8A, 0xBB, replacement); // mov rbx, replacement
        program[0x94] = 0x48;
        program[0x95] = 0x89;
        program[0x96] = 0x18; // mov [rax], rbx
        ASSERT_NO_THROW(emu->write_memory(code, program.data(), program.size()));
        ASSERT_NO_THROW(emu->write_memory(data, &left_value, sizeof(left_value)));
        ASSERT_NO_THROW(emu->write_memory(middle, &old_value, sizeof(old_value)));
        ASSERT_NO_THROW(emu->write_memory(right, &right_value, sizeof(right_value)));

        auto& owner = emu->get_cpu(0);
        auto& peer = emu->get_cpu(1);
        const auto guest_load = [&](x86_64_cpu& cpu, const uint64_t offset) {
            cpu.reg(x86_register::rip, code + offset);
            cpu.start(2);
            return cpu.reg<uint64_t>(x86_register::rax);
        };
        EXPECT_EQ(guest_load(owner, 0x00), left_value);
        EXPECT_EQ(guest_load(peer, 0x20), old_value); // warm peer data TLB
        EXPECT_EQ(guest_load(peer, 0x40), right_value);

        const auto read_on_worker = [&](const size_t index, const uint64_t address, uint64_t& value) {
            emu->set_scheduler_worker_context(index, true);
            const auto clear = utils::finally([&] { emu->set_scheduler_worker_context(index, false); });
            return emu->try_read_memory(address, &value, sizeof(value));
        };
        const auto issued_before = emu->smp_op_watermark();
        uint64_t observed{};
        emu->set_scheduler_worker_context(0, true);
        {
            const auto clear = utils::finally([&] { emu->set_scheduler_worker_context(0, false); });
            ASSERT_TRUE(memory.release_memory(middle, middle_len));
            EXPECT_FALSE(emu->try_read_memory(middle, &observed, sizeof(observed)));
            EXPECT_FALSE(emu->try_read_memory(middle + middle_len / 2, &observed, sizeof(observed)));
            EXPECT_FALSE(emu->try_read_memory(right - sizeof(observed), &observed, sizeof(observed)));
            ASSERT_TRUE(emu->try_read_memory(data, &observed, sizeof(observed)));
            EXPECT_EQ(observed, left_value);
            ASSERT_TRUE(emu->try_read_memory(right, &observed, sizeof(observed)));
            EXPECT_EQ(observed, right_value);
        }
        const auto issued = emu->smp_op_watermark();
        ASSERT_GT(issued, issued_before);
        EXPECT_FALSE(emu->smp_op_applied(issued)) << "peer unmap must remain queued until its safe point";
        ASSERT_TRUE(read_on_worker(1, middle, observed));
        EXPECT_EQ(observed, old_value) << "peer must retain its shared physical page until drain";
        EXPECT_FALSE(emu->smp_op_applied(issued)) << "a read of the retained page must not drain the peer queue";

        // The peer's own safe point retires its old shared-page references.
        ASSERT_NO_THROW(emu->sync_worker_context(1));
        ASSERT_TRUE(emu->smp_op_applied(issued));
        EXPECT_FALSE(read_on_worker(1, middle, observed));
        EXPECT_FALSE(read_on_worker(1, middle + middle_len / 2, observed));
        EXPECT_FALSE(read_on_worker(1, right - sizeof(observed), observed));
        ASSERT_TRUE(read_on_worker(1, data, observed));
        EXPECT_EQ(observed, left_value);
        ASSERT_TRUE(read_on_worker(1, right, observed));
        EXPECT_EQ(observed, right_value);

        // Freshly map the same virtual middle; neighbors retain their bytes.
        ASSERT_TRUE(memory.allocate_memory(middle, middle_len, memory_permission::read_write));
        ASSERT_TRUE(read_on_worker(0, middle, observed));
        EXPECT_EQ(observed, 0U);
        ASSERT_TRUE(read_on_worker(1, middle, observed));
        EXPECT_EQ(observed, 0U);
        EXPECT_EQ(guest_load(peer, 0x00), left_value);
        EXPECT_EQ(guest_load(peer, 0x20), 0U);
        EXPECT_EQ(guest_load(peer, 0x40), right_value);

        peer.reg(x86_register::rip, code + 0x80);
        ASSERT_NO_THROW(peer.start(3));
        ASSERT_TRUE(read_on_worker(0, middle, observed));
        EXPECT_EQ(observed, replacement);
        EXPECT_EQ(guest_load(owner, 0x20), replacement);
    }
}
