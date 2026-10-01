#include <gtest/gtest.h>
#include "../windows-emulator/native_marker_registers.hpp"
#include <cstring>
#include <stdexcept>
#include <vector>

namespace sogen::test
{
    namespace
    {
        class marker_test_memory final : public memory_interface
        {
          public:
            void read_memory(uint64_t, void*, size_t) const override
            {
                throw std::runtime_error("unexpected backend read");
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

        size_t zero_register(const x86_register, void* value, const size_t width)
        {
            std::memset(value, 0, width);
            return width;
        }
    }

    TEST(NativeMarkerRegisters, IssuingReaderAndThreadIdentityTravelWithFixedStack)
    {
        marker_test_memory backend;
        memory_manager memory{backend};
        constexpr uint64_t base = 0x45000000;
        ASSERT_TRUE(memory.allocate_memory(base, 0x1000, memory_permission::read_write));
        size_t register_calls = 0;
        std::vector<uint64_t> addresses;
        const auto snapshot = detail::capture_native_marker_context(
            memory, 7, 52,
            [&](const x86_register reg, void* value, const size_t width) {
                ++register_calls;
                zero_register(reg, value, width);
                if (reg == x86_register::rsp)
                {
                    const auto rsp = base + 0x100;
                    std::memcpy(value, &rsp, sizeof(rsp));
                }
                else if (reg == x86_register::rax)
                {
                    constexpr uint64_t rax = 0x1122334455667788;
                    std::memcpy(value, &rax, sizeof(rax));
                }
                return width;
            },
            [&](const uint64_t address, void* value, const size_t width) {
                addresses.push_back(address);
                EXPECT_EQ(width, 8U);
                const auto word = 0x1234567800000000ULL + addresses.size();
                std::memcpy(value, &word, sizeof(word));
                return true;
            });
        EXPECT_EQ(snapshot.actual_cpu, 7U);
        EXPECT_EQ(snapshot.actual_tid, 52U);
        EXPECT_EQ(register_calls, 78U);
        ASSERT_EQ(addresses.size(), 16U);
        EXPECT_EQ(addresses.front(), base + 0x100);
        EXPECT_EQ(addresses.back(), base + 0x178);
        EXPECT_EQ(snapshot.context.gpr[0], 0x1122334455667788ULL);
        EXPECT_EQ(snapshot.context.stack_requested_words, 16U);
        EXPECT_EQ(snapshot.context.stack_status, 0U);
        EXPECT_EQ(snapshot.context.stack_success[0], 0xFFFFU);
        EXPECT_EQ(snapshot.context.stack_success[1], 0U);
        EXPECT_EQ(snapshot.context.stack[0], 0x1234567800000001ULL);
        EXPECT_EQ(snapshot.context.stack[15], 0x1234567800000010ULL);
    }

    TEST(NativeMarkerRegisters, ShortOrThrowingRegisterReadsCannotExposeWrittenPrefixes)
    {
        marker_test_memory backend;
        memory_manager memory{backend};
        size_t memory_reads = 0;
        const auto snapshot = detail::capture_native_marker_context(
            memory, 3, 48,
            [](const x86_register reg, void* value, const size_t width) {
                std::memset(value, 0xEE, width);
                if (reg == x86_register::rax)
                {
                    return size_t{4};
                }
                if (reg == x86_register::gs_base)
                {
                    throw std::runtime_error("register unavailable");
                }
                if (reg == x86_register::xmm2)
                {
                    return size_t{8};
                }
                return width;
            },
            [&](uint64_t, void*, size_t) {
                ++memory_reads;
                return true;
            });
        EXPECT_EQ(snapshot.context.gpr[0], 0U);
        EXPECT_EQ(snapshot.context.gs_base, 0U);
        EXPECT_EQ(snapshot.context.xmm[2], (std::array<uint8_t, 16>{}));
        const auto rejected = (uint64_t{1} << 0) | (uint64_t{1} << 19) | (uint64_t{1} << 40);
        EXPECT_EQ(snapshot.context.available[0] & rejected, 0U);
        EXPECT_EQ(snapshot.context.read_errors[0] & rejected, rejected);
        EXPECT_EQ(memory_reads, 0U);
        detail::for_each_native_marker_register(snapshot, [&](const auto& field) {
            if (field.name == "rax" || field.name == "gs_base" || field.name == "xmm2")
            {
                EXPECT_FALSE(field.available);
                EXPECT_TRUE(field.read_error);
                EXPECT_TRUE(field.raw.empty());
            }
        });
    }

    TEST(NativeMarkerRegisters, MissingRspPreventsAllStackReads)
    {
        marker_test_memory backend;
        memory_manager memory{backend};
        size_t reads = 0;
        const auto snapshot = detail::capture_native_marker_context(
            memory, 1, 128,
            [](const x86_register reg, void* value, const size_t width) {
                zero_register(reg, value, width);
                return reg == x86_register::rsp ? size_t{0} : width;
            },
            [&](uint64_t, void*, size_t) {
                ++reads;
                return true;
            });
        EXPECT_EQ(reads, 0U);
        EXPECT_EQ(snapshot.context.stack_requested_words, 16U);
        EXPECT_EQ(snapshot.context.stack_status, 2U);
        size_t slots = 0;
        detail::for_each_native_marker_stack_word(snapshot, [&](const auto& word) {
            ++slots;
            EXPECT_FALSE(word.address_available);
            EXPECT_FALSE(word.available);
            EXPECT_TRUE(word.raw.empty());
        });
        EXPECT_EQ(slots, 16U);
    }

    TEST(NativeMarkerRegisters, PartialStackKeepsSlotMaskAndDoesNotClearGuard)
    {
        marker_test_memory backend;
        memory_manager memory{backend};
        constexpr uint64_t base = 0x46000000;
        ASSERT_TRUE(memory.allocate_memory(base, 0x2000, memory_permission::read_write));
        ASSERT_TRUE(memory.protect_memory(base + 0x1000, 0x1000, memory_permission::read_write | memory_permission_ext::guard));
        size_t reads = 0;
        const auto snapshot = detail::capture_native_marker_context(
            memory, 5, 56,
            [](const x86_register reg, void* value, const size_t width) {
                zero_register(reg, value, width);
                if (reg == x86_register::rsp)
                {
                    constexpr uint64_t rsp = base + 0xFF0;
                    std::memcpy(value, &rsp, sizeof(rsp));
                }
                return width;
            },
            [&](const uint64_t address, void* value, const size_t width) {
                ++reads;
                std::memset(value, 0xAB, width);
                return address == base + 0xFF0;
            });
        EXPECT_EQ(reads, 2U);
        EXPECT_EQ(snapshot.context.stack_status, 1U);
        EXPECT_EQ(snapshot.context.stack_success[0], 1U);
        EXPECT_EQ(snapshot.context.stack[0], 0xABABABABABABABABULL);
        EXPECT_EQ(snapshot.context.stack[1], 0U);
        EXPECT_TRUE(memory.get_region_info(base + 0x1000).permissions.is_guarded());
        detail::for_each_native_marker_stack_word(snapshot, [&](const auto& word) {
            EXPECT_EQ(word.available, word.index == 0);
            EXPECT_EQ(word.raw.size(), word.index == 0 ? 8U : 0U);
        });
    }

    TEST(NativeMarkerRegisters, ReaderBudgetRejectionNeverPublishesAnUnconfirmedStackWord)
    {
        marker_test_memory backend;
        memory_manager memory{backend};
        constexpr uint64_t base = 0x47000000;
        ASSERT_TRUE(memory.allocate_memory(base, 0x1000, memory_permission::read_write));
        size_t attempts = 0;
        size_t backend_reads = 0;
        const auto snapshot = detail::capture_native_marker_context(
            memory, 6, 64,
            [](const x86_register reg, void* value, const size_t width) {
                zero_register(reg, value, width);
                if (reg == x86_register::rsp)
                {
                    constexpr uint64_t rsp = base;
                    std::memcpy(value, &rsp, sizeof(rsp));
                }
                return width;
            },
            [&](uint64_t, void* value, size_t width) {
                ++attempts;
                if (backend_reads == 3)
                {
                    return false;
                }
                ++backend_reads;
                std::memset(value, 0x5A, width);
                return true;
            });
        EXPECT_EQ(attempts, 16U);
        EXPECT_EQ(backend_reads, 3U);
        EXPECT_EQ(snapshot.context.stack_status, 1U);
        EXPECT_EQ(snapshot.context.stack_success[0], 7U);
        EXPECT_EQ(snapshot.context.stack[3], 0U);
    }

    TEST(NativeMarkerRegisters, FormatterRetainsVectorWidthAndMasksStaleUnavailableBytes)
    {
        detail::native_marker_register_snapshot snapshot{.actual_cpu = 2, .actual_tid = 132};
        snapshot.context.gpr[0] = UINT64_MAX;
        snapshot.context.read_errors[0] = 1;
        snapshot.context.available[0] = uint64_t{1} << 41;
        for (size_t index = 0; index < 16; ++index)
        {
            snapshot.context.xmm[3][index] = static_cast<uint8_t>(index);
        }
        size_t fields = 0;
        detail::for_each_native_marker_register(snapshot, [&](const auto& field) {
            ++fields;
            if (field.name == "rax")
            {
                EXPECT_FALSE(field.available);
                EXPECT_TRUE(field.read_error);
                EXPECT_TRUE(field.raw.empty());
                EXPECT_TRUE(detail::native_marker_raw_hex(field.raw).empty());
            }
            else if (field.name == "xmm3")
            {
                EXPECT_TRUE(field.available);
                EXPECT_EQ(field.width, 16U);
                EXPECT_EQ(field.raw.size(), 16U);
                EXPECT_EQ(detail::native_marker_raw_hex(field.raw), "000102030405060708090a0b0c0d0e0f");
            }
            else
            {
                EXPECT_FALSE(field.available);
                EXPECT_TRUE(field.raw.empty());
            }
        });
        EXPECT_EQ(fields, 78U);
    }

    TEST(NativeMarkerRegisters, AdmissionBeforeMetadataCanPreventAllStackReads)
    {
        marker_test_memory backend;
        memory_manager memory{backend};
        constexpr uint64_t base = 0x48000000;
        ASSERT_TRUE(memory.allocate_memory(base, 0x1000, memory_permission::read_write | memory_permission_ext::guard));
        size_t checks = 0;
        size_t reads = 0;
        const auto snapshot = detail::capture_native_marker_context(
            memory, 4, 56,
            [](const x86_register reg, void* value, const size_t width) {
                zero_register(reg, value, width);
                if (reg == x86_register::rsp)
                {
                    constexpr uint64_t rsp = base;
                    std::memcpy(value, &rsp, sizeof(rsp));
                }
                return width;
            },
            [&](uint64_t, void*, size_t) {
                ++reads;
                return true;
            },
            [&](const uint64_t address, const size_t width) {
                EXPECT_EQ(address, base + checks * 8);
                EXPECT_EQ(width, 8U);
                ++checks;
                return false;
            });
        EXPECT_EQ(checks, 16U);
        EXPECT_EQ(reads, 0U);
        EXPECT_EQ(snapshot.context.stack_status, 2U);
        EXPECT_EQ(snapshot.context.stack_success[0], 0U);
        EXPECT_TRUE(memory.get_region_info(base).permissions.is_guarded());
    }

    TEST(NativeMarkerRegisters, AdmissionAfterMetadataCanRejectBackendReadWithoutChargingTwice)
    {
        marker_test_memory backend;
        memory_manager memory{backend};
        constexpr uint64_t base = 0x49000000;
        ASSERT_TRUE(memory.allocate_memory(base, 0x1000, memory_permission::read_write));
        size_t checks = 0;
        size_t charged_reads = 0;
        const auto snapshot = detail::capture_native_marker_context(
            memory, 2, 132,
            [](const x86_register reg, void* value, const size_t width) {
                zero_register(reg, value, width);
                if (reg == x86_register::rsp)
                {
                    constexpr uint64_t rsp = base;
                    std::memcpy(value, &rsp, sizeof(rsp));
                }
                return width;
            },
            [&](uint64_t, void*, size_t) {
                ++charged_reads;
                return true;
            },
            [&](uint64_t, size_t) { return ++checks % 2 == 1; });
        EXPECT_EQ(checks, 32U);
        EXPECT_EQ(charged_reads, 0U);
        EXPECT_EQ(snapshot.context.stack_status, 2U);
        EXPECT_EQ(snapshot.context.stack_success[0], 0U);
    }

    TEST(NativeMarkerRegisters, ThrowingStackReaderLeavesOnlyConfirmedSlotsAvailable)
    {
        marker_test_memory backend;
        memory_manager memory{backend};
        constexpr uint64_t base = 0x4A000000;
        ASSERT_TRUE(memory.allocate_memory(base, 0x1000, memory_permission::read_write));
        size_t attempts = 0;
        const auto snapshot = detail::capture_native_marker_context(
            memory, 3, 48,
            [](const x86_register reg, void* value, const size_t width) {
                zero_register(reg, value, width);
                if (reg == x86_register::rsp)
                {
                    constexpr uint64_t rsp = base;
                    std::memcpy(value, &rsp, sizeof(rsp));
                }
                return width;
            },
            [&](uint64_t, void* value, const size_t width) {
                std::memset(value, 0xFF, width);
                if (++attempts > 1)
                {
                    throw std::runtime_error("read failed");
                }
                return true;
            });
        EXPECT_EQ(attempts, 16U);
        EXPECT_EQ(snapshot.context.stack_status, 1U);
        EXPECT_EQ(snapshot.context.stack_success[0], 1U);
        EXPECT_EQ(snapshot.context.stack[0], UINT64_MAX);
        EXPECT_EQ(snapshot.context.stack[1], 0U);
    }
}
