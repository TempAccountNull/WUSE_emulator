#include <gtest/gtest.h>
#include "../windows-emulator/guest_inspection_query.hpp"
#include "../windows-emulator/guest_inspection_shared_memory.hpp"
#include "../windows-emulator/guest_memory_shared_memory.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace sogen::test
{
    namespace
    {
        class inspection_test_memory final : public memory_interface
        {
          public:
            void read_memory(uint64_t, void*, size_t) const override
            {
                throw std::runtime_error("unexpected guest read");
            }

            bool try_read_memory(uint64_t, void*, size_t) const override
            {
                return false;
            }

            void write_memory(uint64_t, const void*, size_t) override
            {
            }

            bool try_write_memory(uint64_t, const void*, size_t) override
            {
                return false;
            }

          private:
            void map_mmio(uint64_t, size_t, mmio_read_callback, mmio_write_callback) override
            {
            }

            void map_memory(uint64_t, size_t, memory_permission) override
            {
            }

            void unmap_memory(uint64_t, size_t) override
            {
            }

            void apply_memory_protection(uint64_t, size_t, memory_permission) override
            {
            }
        };

        detail::guest_inspection_packet request(const detail::inspection_operation operation, const uint32_t limit = 1)
        {
            detail::guest_inspection_packet packet{};
            packet.operation = static_cast<uint32_t>(operation);
            packet.request_id = 7;
            packet.expected_pid = 11;
            packet.expected_birth = 22;
            packet.expected_generation = 33;
            packet.requested_cpu = 3;
            packet.requested_tid = 77;
            packet.limit = limit;
            return packet;
        }
    }

    TEST(GuestInspection, WireBoundsPreserveLegacyPacket)
    {
        EXPECT_EQ(sizeof(detail::guest_memory_packet), 4256U);
        EXPECT_EQ(sizeof(detail::guest_inspection_packet), 4096U);
        EXPECT_EQ(offsetof(detail::guest_inspection_packet, payload), 128U);
        EXPECT_EQ(offsetof(detail::inspection_context, stack_success), 752U);
        EXPECT_EQ(sizeof(detail::inspection_context), 1792U);
        EXPECT_LE(sizeof(detail::inspection_region_page), 3968U);
    }

    TEST(GuestInspection, ProducerIdentityAndRestoreGenerationRejectStaleRequests)
    {
        detail::guest_sampling_admission admission;
        detail::inspection_service_state service{{11, 22, 33}, admission};
        auto packet = request(detail::inspection_operation::address);
        packet.producer_birth = 999;
        packet.producer_generation = 999;
        size_t calls = 0;
        const auto reader = [&](detail::guest_inspection_packet& response) {
            ++calls;
            response.status = 0;
            response.payload[0] = 0x55;
        };
        EXPECT_TRUE(service.service(packet, {3, 77}, 1, reader));
        EXPECT_EQ(calls, 1U);
        EXPECT_EQ(packet.producer_birth, 22U);
        EXPECT_EQ(packet.producer_generation, 33U);
        service.set_generation(34);
        ++packet.request_id;
        EXPECT_TRUE(service.service(packet, {3, 77}, 100000001, reader));
        EXPECT_EQ(calls, 1U);
        EXPECT_EQ(packet.status, 5U);
        EXPECT_EQ(packet.producer_generation, 34U);
        EXPECT_TRUE(std::ranges::all_of(packet.payload, [](const auto value) { return value == 0; }));
        EXPECT_EQ(packet.sample_sequence, 0U);
    }

    TEST(GuestInspection, WrongOwnerWaitsAndUnsafeOrMismatchedOwnerClearsPayload)
    {
        detail::guest_sampling_admission admission;
        detail::inspection_service_state service{{11, 22, 33}, admission};
        auto packet = request(detail::inspection_operation::context, 128);
        packet.payload.fill(0xFE);
        size_t calls = 0;
        const auto reader = [&](detail::guest_inspection_packet&) { ++calls; };
        EXPECT_FALSE(service.service(packet, {2, 77}, 1, reader));
        EXPECT_EQ(packet.response_id, 0U);
        EXPECT_EQ(calls, 0U);
        const std::array owners = {detail::inspection_owner{3, 78}, detail::inspection_owner{3, 77, true},
                                   detail::inspection_owner{3, 77, false, true}, detail::inspection_owner{3, 0}};
        for (const auto owner : owners)
        {
            ++packet.request_id;
            packet.payload.fill(0xFE);
            EXPECT_TRUE(service.service(packet, owner, 1, reader));
            EXPECT_EQ(packet.status, 6U);
            EXPECT_EQ(packet.actual_cpu, UINT32_MAX);
            EXPECT_EQ(packet.actual_tid, 0U);
            EXPECT_TRUE(std::ranges::all_of(packet.payload, [](const auto value) { return value == 0; }));
        }
        EXPECT_EQ(calls, 0U);
    }

    TEST(GuestInspection, InvalidBoundsAndRepeatedOrPartialPublicationDoNotRead)
    {
        detail::guest_sampling_admission admission;
        detail::inspection_service_state service{{11, 22, 33}, admission};
        auto packet = request(detail::inspection_operation::context, 129);
        size_t calls = 0;
        const auto reader = [&](detail::guest_inspection_packet&) { ++calls; };
        EXPECT_TRUE(service.service(packet, {3, 77}, 1, reader));
        EXPECT_EQ(packet.status, 2U);
        packet.response_id = 0;
        EXPECT_FALSE(service.service(packet, {3, 77}, 100000001, reader));
        ++packet.request_id;
        packet.packet_size = 4097;
        EXPECT_FALSE(service.service(packet, {3, 77}, 100000001, reader));
        packet.packet_size = 4096;
        packet.operation = static_cast<uint32_t>(detail::inspection_operation::regions);
        packet.limit = 65;
        EXPECT_TRUE(service.service(packet, {3, 77}, 100000001, reader));
        EXPECT_EQ(packet.status, 2U);
        EXPECT_EQ(calls, 0U);
    }

    TEST(GuestInspection, SharedAdmissionPreservesFiftyMillisecondBoundaryAcrossOperations)
    {
        detail::guest_sampling_admission admission;
        detail::inspection_service_state service{{11, 22, 33}, admission};
        EXPECT_TRUE(admission.try_admit(0));
        auto packet = request(detail::inspection_operation::address);
        size_t calls = 0;
        const auto reader = [&](detail::guest_inspection_packet& response) {
            ++calls;
            response.status = 0;
        };
        EXPECT_TRUE(service.service(packet, {3, 77}, 49999999, reader));
        EXPECT_EQ(packet.status, 4U);
        ++packet.request_id;
        EXPECT_TRUE(service.service(packet, {3, 77}, 50000000, reader));
        EXPECT_EQ(packet.status, 0U);
        EXPECT_FALSE(admission.try_admit(99999999));
        EXPECT_TRUE(admission.try_admit(100000000));
        ++packet.request_id;
        packet.operation = static_cast<uint32_t>(detail::inspection_operation::context);
        EXPECT_TRUE(service.service(packet, {3, 77}, 149999999, reader));
        EXPECT_EQ(packet.status, 4U);
        ++packet.request_id;
        EXPECT_TRUE(service.service(packet, {3, 77}, 150000000, reader));
        EXPECT_EQ(packet.status, 0U);
        EXPECT_EQ(packet.actual_cpu, 3U);
        EXPECT_EQ(packet.actual_tid, 77U);
        EXPECT_EQ(calls, 2U);
    }

    TEST(GuestInspection, CanonicalRegionsPreserveCommitHolesAndGuardMetadata)
    {
        inspection_test_memory backend;
        memory_manager memory{backend};
        constexpr uint64_t base = 0x44000000;
        ASSERT_TRUE(memory.allocate_memory(base, 0x5000, memory_permission::read_write, true));
        ASSERT_TRUE(memory.commit_memory(base + 0x1000, 0x1000, memory_permission::read_write | memory_permission_ext::guard));
        ASSERT_TRUE(memory.commit_memory(base + 0x4000, 0x1000, memory_permission::read));
        const auto version = memory.get_layout_version();
        detail::inspection_region row{};
        ASSERT_TRUE(detail::inspection_region_at(memory, base + 0x1080, row));
        EXPECT_EQ(row.base, base + 0x1000);
        EXPECT_EQ(row.length, 0x1000U);
        EXPECT_EQ(row.allocation_base, base);
        EXPECT_EQ(row.allocation_length, 0x5000U);
        EXPECT_EQ(row.queried_page_base, base + 0x1000);
        EXPECT_EQ(row.guarded, 1U);
        ASSERT_TRUE(detail::inspection_region_at(memory, base + 0x3080, row));
        EXPECT_EQ(row.base, base + 0x2000);
        EXPECT_EQ(row.length, 0x2000U);
        EXPECT_EQ(row.queried_page_base, base + 0x3000);
        EXPECT_EQ(row.forward_length, 0x1000U);
        EXPECT_EQ(row.state, 1U);
        detail::inspection_region_page page{};
        uint64_t cursor{};
        detail::inspection_regions(memory, base, 2, page, cursor);
        ASSERT_EQ(page.count, 2U);
        EXPECT_EQ(cursor, base + 0x2000);
        EXPECT_EQ(page.regions[0].state, 1U);
        EXPECT_EQ(page.regions[1].state, 2U);
        EXPECT_FALSE(detail::inspection_region_at(memory, MAX_ALLOCATION_END_EXCL, row));
        EXPECT_FALSE(detail::inspection_region_at(memory, UINT64_MAX, row));
        EXPECT_EQ(memory.get_layout_version(), version);
        EXPECT_TRUE(memory.get_region_info(base + 0x1000).permissions.is_guarded());
    }

    TEST(GuestInspection, NamedRegistersAcceptOnlyExactWidthsAndNeverReadSyntheticYmm)
    {
        detail::inspection_context result{};
        std::vector<x86_register> seen{};
        detail::inspection_registers(
            [&](const x86_register reg, void* value, const size_t width) {
                seen.push_back(reg);
                std::memset(value, 0xA5, width);
                if (reg == x86_register::fs)
                {
                    return size_t{0};
                }
                if (reg == x86_register::cr3)
                {
                    throw std::runtime_error("unsupported");
                }
                return width;
            },
            result);
        EXPECT_EQ(seen.size(), 78U);
        EXPECT_EQ(result.gpr[0], 0xA5A5A5A5A5A5A5A5ULL);
        EXPECT_EQ(result.rflags, 0xA5A5A5A5A5A5A5A5ULL);
        EXPECT_EQ(result.flags_view, 1U);
        EXPECT_EQ(result.segments[4], 0U);
        EXPECT_FALSE(result.available[0] & (uint64_t{1} << 34));
        EXPECT_TRUE(result.read_errors[0] & (uint64_t{1} << 34));
        EXPECT_FALSE(result.available[0] & (uint64_t{1} << 22));
        EXPECT_TRUE(std::ranges::all_of(result.xmm[15], [](const auto value) { return value == 0xA5; }));
        EXPECT_TRUE(std::ranges::none_of(seen, [](const auto reg) { return reg >= x86_register::ymm0 && reg <= x86_register::ymm31; }));
    }

    TEST(GuestInspection, NonzeroOwnerProducesDistinctNamedRegisterFieldsWithoutReadingAnotherCpu)
    {
        detail::guest_sampling_admission admission;
        detail::inspection_service_state service{{11, 22, 33}, admission};
        auto packet = request(detail::inspection_operation::context, 0);
        size_t getter_calls = 0;
        const auto reader = [&](detail::guest_inspection_packet& response) {
            detail::inspection_context context{};
            detail::inspection_registers(
                [&](const x86_register reg, void* value, const size_t width) {
                    ++getter_calls;
                    const auto pattern = static_cast<uint8_t>(static_cast<uint32_t>(reg) + 3);
                    std::memset(value, pattern, width);
                    return width;
                },
                context);
            std::memcpy(response.payload.data(), &context, sizeof(context));
            response.status = 0;
        };
        EXPECT_FALSE(service.service(packet, {0, 77}, 1, reader));
        EXPECT_EQ(getter_calls, 0U);
        ASSERT_TRUE(service.service(packet, {3, 77}, 1, reader));
        EXPECT_EQ(getter_calls, 78U);
        EXPECT_EQ(packet.actual_cpu, 3U);
        EXPECT_EQ(packet.actual_tid, 77U);
        detail::inspection_context context{};
        std::memcpy(&context, packet.payload.data(), sizeof(context));
        constexpr std::array expected = {x86_register::rax, x86_register::rbx, x86_register::rcx, x86_register::rdx,
                                         x86_register::rsi, x86_register::rdi, x86_register::rbp, x86_register::rsp,
                                         x86_register::r8,  x86_register::r9,  x86_register::r10, x86_register::r11,
                                         x86_register::r12, x86_register::r13, x86_register::r14, x86_register::r15};
        for (size_t index = 0; index < expected.size(); ++index)
        {
            const auto pattern = static_cast<uint8_t>(static_cast<uint32_t>(expected[index]) + 3);
            EXPECT_EQ(context.gpr[index], uint64_t{pattern} * 0x0101010101010101ULL);
        }
        EXPECT_EQ(context.flags_view, 1U);
    }

    TEST(GuestInspection, StackWordsRespectGuardsMmioAndFailedReadWithoutExposingPrefix)
    {
        inspection_test_memory backend;
        memory_manager memory{backend};
        constexpr uint64_t base = 0x45000000;
        ASSERT_TRUE(memory.allocate_memory(base, 0x2000, memory_permission::read_write));
        ASSERT_TRUE(memory.protect_memory(base + 0x1000, 0x1000, memory_permission::read_write | memory_permission_ext::guard));
        size_t calls = 0;
        detail::inspection_context result{};
        result.available[0] = uint64_t{1} << 7;
        result.gpr[7] = base + 0xFF0;
        detail::inspection_stack(
            memory,
            [&](const uint64_t address, void* value, const size_t size) {
                ++calls;
                std::memset(value, 0xAB, size);
                return address == base + 0xFF0;
            },
            4, result);
        EXPECT_EQ(calls, 2U);
        EXPECT_EQ(result.stack_status, 1U);
        EXPECT_EQ(result.stack_success[0], 1U);
        EXPECT_EQ(result.stack[0], 0xABABABABABABABABULL);
        EXPECT_EQ(result.stack[1], 0U);
        EXPECT_EQ(result.stack[2], 0U);
        EXPECT_TRUE(memory.get_region_info(base + 0x1000).permissions.is_guarded());
        ASSERT_TRUE(memory.allocate_mmio(base + 0x10000, 0x1000, [](uint64_t, void*, size_t) {}, [](uint64_t, const void*, size_t) {}));
        detail::inspection_context mmio{};
        mmio.available[0] = uint64_t{1} << 7;
        mmio.gpr[7] = base + 0x10000;
        detail::inspection_stack(
            memory,
            [&](uint64_t, void*, size_t) {
                ++calls;
                return true;
            },
            128, mmio);
        EXPECT_EQ(calls, 2U);
        EXPECT_EQ(mmio.stack_status, 2U);
        EXPECT_EQ(mmio.stack_success[0], 0U);
        EXPECT_EQ(mmio.stack_success[1], 0U);
    }
}
