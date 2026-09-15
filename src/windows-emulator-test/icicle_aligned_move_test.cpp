#include "emulation_test_utils.hpp"
#include <memory_manager.hpp>
#include <array>
#include <vector>
#include <algorithm>
#include <chrono>

namespace sogen::test
{
    class IcicleAlignedMove : public testing::Test
    {
      protected:
        using vector = std::array<uint8_t, 64>;
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};
        uint64_t data{};
        size_t cursor{};

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x4000, memory_permission::all);
            data = memory->allocate_memory(0x2000, memory_permission::read_write);
            ASSERT_NE(code, 0u);
            ASSERT_NE(data, 0u);
            emu->hook_interrupt([](cpu_interface& cpu, int) { cpu.stop(); });
            xcr0(0xE7);
        }

        uint64_t load(std::vector<uint8_t> bytes)
        {
            const auto address = code + cursor;
            cursor += 32;
            bytes.push_back(0x90);
            emu->write_memory(address, bytes.data(), bytes.size());
            emu->reg(x86_register::rip, address);
            return address;
        }

        void xcr0(const uint64_t value)
        {
            load({0x0F, 0x01, 0xD1});
            emu->reg(x86_register::rax, value);
            emu->reg(x86_register::rcx, 0ULL);
            emu->reg(x86_register::rdx, 0ULL);
            emu->start(1);
        }

        static x86_register zmm(const size_t index)
        {
            return static_cast<x86_register>(static_cast<int>(x86_register::zmm0) + static_cast<int>(index));
        }

        static vector pattern(const uint8_t seed)
        {
            vector result{};
            for (size_t index = 0; index < result.size(); ++index)
            {
                result[index] = static_cast<uint8_t>(seed + index * 7);
            }
            return result;
        }

        static std::vector<uint8_t> evex(const size_t width, const size_t element, const size_t reg, const size_t rm, const uint8_t mask,
                                         const bool zero, const bool store = false, const bool memory_operand = false)
        {
            const auto p0 = static_cast<uint8_t>(0xF1 ^ ((reg & 8) << 4) ^ (reg & 16) ^ ((rm & 8) << 2) ^ ((rm & 16) << 2));
            const auto p1 = static_cast<uint8_t>(element == 8 ? 0xFD : 0x7D);
            uint8_t length{};
            if (width == 32)
            {
                length = 0x20;
            }
            if (width == 64)
            {
                length = 0x40;
            }
            return {0x62,
                    p0,
                    p1,
                    static_cast<uint8_t>(8 | length | mask | (zero ? 0x80 : 0)),
                    static_cast<uint8_t>(store ? 0x7F : 0x6F),
                    static_cast<uint8_t>((memory_operand ? 0 : 0xC0) | ((reg & 7) << 3) | (rm & 7))};
        }

        void expect_interrupt(const std::vector<uint8_t>& bytes, const int expected)
        {
            int received{};
            auto* const hook = emu->hook_interrupt([&](cpu_interface& cpu, const int value) {
                received = value;
                cpu.stop();
            });
            const auto address = load(bytes);
            emu->start(1);
            EXPECT_EQ(received, expected);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address);
            emu->delete_hook(hook);
        }
    };

    TEST_F(IcicleAlignedMove, LegacyRegisterCopiesPreserveUpperStateAndFlags)
    {
        for (const auto opcode : {0x6F, 0x7F})
        {
            const auto original = pattern(1);
            const auto source = pattern(87);
            emu->reg<vector>(zmm(1), original);
            emu->reg<vector>(zmm(2), source);
            emu->reg(x86_register::eflags, 0xCD5u);
            emu->reg(x86_register::mxcsr, 0xFFFFu);
            const auto flags = emu->reg<uint32_t>(x86_register::eflags);
            const auto address = load({0x66, 0x0F, static_cast<uint8_t>(opcode), static_cast<uint8_t>(opcode == 0x6F ? 0xCA : 0xD1)});
            emu->start(1);
            auto expected = original;
            std::copy_n(source.begin(), 16, expected.begin());
            EXPECT_EQ(emu->reg<vector>(zmm(1)), expected);
            EXPECT_EQ(emu->reg<vector>(zmm(2)), source);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + 4);
            EXPECT_EQ(emu->reg<uint32_t>(x86_register::eflags), flags);
            EXPECT_EQ(emu->reg<uint32_t>(x86_register::mxcsr), 0xFFFFu);
        }
    }

    TEST_F(IcicleAlignedMove, VexBothWidthsAndOpcodesClearUpperZmm)
    {
        for (const auto width : {16u, 32u})
        {
            for (const auto opcode : {0x6Fu, 0x7Fu})
            {
                const auto source = pattern(93);
                emu->reg<vector>(zmm(1), pattern(1));
                emu->reg<vector>(zmm(2), source);
                const auto address = load({0xC5, static_cast<uint8_t>(width == 16 ? 0xF9 : 0xFD), static_cast<uint8_t>(opcode),
                                           static_cast<uint8_t>(opcode == 0x6F ? 0xCA : 0xD1)});
                emu->start(1);
                vector expected{};
                std::copy_n(source.begin(), width, expected.begin());
                EXPECT_EQ(emu->reg<vector>(zmm(1)), expected);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + 4);
            }
        }
    }

    TEST_F(IcicleAlignedMove, EvexAllWidthsElementsAndMasks)
    {
        const auto source = pattern(61);
        const auto original = pattern(173);
        for (const auto width : {16u, 32u, 64u})
        {
            for (const auto element : {4u, 8u})
            {
                for (const bool store : {false, true})
                {
                    for (const bool zero : {false, true})
                    {
                        emu->reg<vector>(zmm(17), original);
                        emu->reg<vector>(zmm(31), source);
                        emu->reg(x86_register::k3, 0xA55AULL);
                        const auto bytes = evex(width, element, store ? 31 : 17, store ? 17 : 31, 3, zero, store);
                        const auto address = load(bytes);
                        emu->start(1);
                        auto expected = original;
                        for (size_t i = 0; i < width; ++i)
                        {
                            if ((0xA55A >> (i / element)) & 1)
                            {
                                expected[i] = source[i];
                            }
                            else if (zero)
                            {
                                expected[i] = 0;
                            }
                        }
                        std::fill(expected.begin() + width, expected.end(), 0);
                        EXPECT_EQ(emu->reg<vector>(zmm(17)), expected);
                        EXPECT_EQ(emu->reg<vector>(zmm(31)), source);
                        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
                        EXPECT_EQ(emu->reg<uint64_t>(x86_register::k3), 0xA55Au);
                    }
                }
            }
        }
    }

    TEST_F(IcicleAlignedMove, EvexEveryRegisterNumberAndK0MeansUnmasked)
    {
        emu->reg(x86_register::k0, 0ULL);
        for (size_t index = 0; index < 32; ++index)
        {
            const auto source_index = 31 - index;
            const auto source = pattern(static_cast<uint8_t>(index));
            emu->reg<vector>(zmm(index), pattern(250));
            emu->reg<vector>(zmm(source_index), source);
            load(evex(64, 4, index, source_index, 0, false));
            emu->start(1);
            EXPECT_EQ(emu->reg<vector>(zmm(index)), source);
        }
    }

    TEST_F(IcicleAlignedMove, EvexMaskedLoadsAndStoresAtEveryWidth)
    {
        const auto source = pattern(32);
        const auto initial = pattern(166);
        for (const auto width : {16u, 32u, 64u})
        {
            for (const auto element : {4u, 8u})
            {
                for (const bool store : {false, true})
                {
                    emu->reg<vector>(zmm(25), source);
                    emu->reg(x86_register::rax, data);
                    emu->reg(x86_register::k7, 0x9696ULL);
                    emu->write_memory(data, initial.data(), initial.size());
                    load(evex(width, element, 25, 0, 7, false, store, true));
                    emu->start(1);
                    auto expected = store ? initial : source;
                    for (size_t i = 0; i < width; ++i)
                    {
                        if ((0x9696 >> (i / element)) & 1)
                        {
                            expected[i] = store ? source[i] : initial[i];
                        }
                    }
                    if (store)
                    {
                        EXPECT_EQ(emu->read_memory<vector>(data), expected);
                    }
                    else
                    {
                        std::fill(expected.begin() + width, expected.end(), 0);
                        EXPECT_EQ(emu->reg<vector>(zmm(25)), expected);
                    }
                }
            }
        }
    }

    TEST_F(IcicleAlignedMove, EvexCompressedDisplacementsAndSib)
    {
        for (const auto width : {16u, 32u, 64u})
        {
            const auto source = pattern(42);
            emu->reg(x86_register::r12, data + 0x100);
            emu->reg(x86_register::r9, 0x20ULL);
            emu->write_memory(data + 0x100 + 0x40 - 2 * width, source.data(), width);
            auto bytes = evex(width, 4, 27, 12, 0, false, false, true);
            bytes[1] ^= 0x40;
            bytes[5] |= 0x40;
            bytes.push_back(0x4C);
            bytes.push_back(0xFE);
            const auto address = load(bytes);
            emu->start(1);
            vector expected{};
            std::copy_n(source.begin(), width, expected.begin());
            EXPECT_EQ(emu->reg<vector>(zmm(27)), expected);
            EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
        }
    }

    TEST_F(IcicleAlignedMove, AllMemoryFormsRejectMisalignmentBeforeChangingState)
    {
        std::vector<std::vector<uint8_t>> forms{
            {0x66, 0x0F, 0x6F, 0x08}, {0x66, 0x0F, 0x7F, 0x08}, {0xC5, 0xF9, 0x6F, 0x08}, {0xC5, 0xFD, 0x7F, 0x08}};
        for (const auto width : {16u, 32u, 64u})
        {
            for (const auto element : {4u, 8u})
            {
                for (const bool store : {false, true})
                {
                    forms.push_back(evex(width, element, 1, 0, 1, false, store, true));
                }
            }
        }
        emu->reg(x86_register::rax, data + 1);
        emu->reg(x86_register::k1, 0ULL);
        const auto initial = pattern(72);
        emu->reg<vector>(zmm(1), initial);
        emu->write_memory(data, initial.data(), initial.size());
        for (const auto& bytes : forms)
        {
            expect_interrupt(bytes, 13);
            EXPECT_EQ(emu->reg<vector>(zmm(1)), initial);
            EXPECT_EQ(emu->read_memory<vector>(data), initial);
        }
    }

    TEST_F(IcicleAlignedMove, ZeroMaskSuppressesPageAndCanonicalFaults)
    {
        emu->reg(x86_register::k2, 0ULL);
        for (const auto pointer : {0x100000000000ULL, 0x800000000000ULL})
        {
            for (const bool store : {false, true})
            {
                const auto initial = pattern(89);
                emu->reg<vector>(zmm(20), initial);
                emu->reg(x86_register::rax, pointer);
                const auto bytes = evex(64, 8, 20, 0, 2, !store, store, true);
                const auto address = load(bytes);
                emu->start(1);
                EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
                EXPECT_EQ(emu->reg<vector>(zmm(20)), store ? initial : vector{});
            }
        }
    }

    TEST_F(IcicleAlignedMove, NonCanonicalStackAndDataFaults)
    {
        emu->reg(x86_register::rax, 0x800000000000ULL);
        emu->reg(x86_register::rbp, 0x800000000000ULL);
        expect_interrupt({0x66, 0x0F, 0x6F, 0x08}, 13);
        expect_interrupt({0x66, 0x0F, 0x6F, 0x4D, 0}, 12);
        expect_interrupt(evex(64, 4, 1, 0, 0, false, false, true), 13);
    }

    TEST_F(IcicleAlignedMove, ReservedEncodingsRaiseUndefinedInstruction)
    {
        const auto valid = evex(64, 4, 1, 2, 0, false);
        for (const auto [byte, bits] :
             std::array<std::pair<size_t, uint8_t>, 7>{{{1, 8}, {2, 4}, {2, 8}, {3, 8}, {3, 16}, {3, 32}, {3, 128}}})
        {
            auto invalid = valid;
            invalid[byte] ^= bits;
            expect_interrupt(invalid, 6);
        }
        expect_interrupt(evex(64, 8, 1, 0, 1, true, true, true), 6);
        expect_interrupt({0xC5, 0xE9, 0x6F, 0xCA}, 6);
        expect_interrupt({0x66, 0xC5, 0xF9, 0x6F, 0xCA}, 6);
        auto invalid = valid;
        invalid.insert(invalid.begin(), 0x66);
        expect_interrupt(invalid, 6);
    }

    TEST_F(IcicleAlignedMove, DisabledStateAndTaskSwitchedFaultBeforeMemory)
    {
        emu->reg(x86_register::rax, 0x100000000000ULL);
        xcr0(3);
        expect_interrupt({0xC5, 0xF9, 0x6F, 0x08}, 6);
        expect_interrupt(evex(64, 4, 1, 0, 0, false, false, true), 6);
        for (const auto missing : {1ULL, 2ULL, 4ULL, 0x20ULL, 0x40ULL, 0x80ULL})
        {
            xcr0(0xE7 & ~missing);
            expect_interrupt(evex(16, 8, 1, 0, 0, false, false, true), 6);
        }
        xcr0(0xE7);
        const auto cr0 = emu->reg<uint64_t>(x86_register::cr0);
        emu->reg(x86_register::cr0, cr0 | 8);
        expect_interrupt({0x66, 0x0F, 0x6F, 0x08}, 7);
        expect_interrupt({0xC5, 0xFD, 0x6F, 0x08}, 7);
        expect_interrupt(evex(64, 4, 1, 0, 0, false, false, true), 7);
        emu->reg(x86_register::cr0, cr0 | 4);
        expect_interrupt({0x66, 0x0F, 0x6F, 0x08}, 6);
    }

    TEST_F(IcicleAlignedMove, ExtendedRegisterStateSurvivesThreadContextAndSnapshot)
    {
        for (size_t i = 0; i < 32; ++i)
        {
            emu->reg<vector>(zmm(i), pattern(static_cast<uint8_t>(i)));
        }
        emu->reg(x86_register::k7, 0x123456789ABCDEF0ULL);
        const auto saved = emu->save_registers();
        for (size_t i = 0; i < 32; ++i)
        {
            emu->reg<vector>(zmm(i), vector{});
        }
        emu->reg(x86_register::k7, 0ULL);
        emu->restore_registers(saved);
        for (size_t i = 0; i < 32; ++i)
        {
            EXPECT_EQ(emu->reg<vector>(zmm(i)), pattern(static_cast<uint8_t>(i)));
        }
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::k7), 0x123456789ABCDEF0ULL);
    }

    TEST_F(IcicleAlignedMove, CapturedMemsetStoreAndPageBoundary)
    {
        const auto source = pattern(55);
        emu->reg<vector>(zmm(0), source);
        emu->reg(x86_register::rcx, data + 0x1000 - 16);
        const auto address = load({0x66, 0x0F, 0x7F, 0x41, 0x10});
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + 5);
        EXPECT_EQ((emu->read_memory<std::array<uint8_t, 16>>(data + 0x1000)),
                  (std::array<uint8_t, 16>{55, 62, 69, 76, 83, 90, 97, 104, 111, 118, 125, 132, 139, 146, 153, 160}));
        EXPECT_EQ(emu->reg<vector>(zmm(0)), source);
    }

    TEST_F(IcicleAlignedMove, LegacyAndVexAlignedMemoryLoadsAndStores)
    {
        const auto source = pattern(66);
        for (const size_t width : {16u, 32u})
        {
            for (const bool store : {false, true})
            {
                const auto initial = pattern(199);
                emu->reg<vector>(zmm(9), source);
                emu->reg(x86_register::r12, data);
                emu->write_memory(data, initial.data(), initial.size());
                const auto bytes = std::vector<uint8_t>{
                    0xC4, 0x41, static_cast<uint8_t>(width == 16 ? 0x79 : 0x7D), static_cast<uint8_t>(store ? 0x7F : 0x6F), 0x0C, 0x24};
                load(bytes);
                emu->start(1);
                auto expected = store ? initial : vector{};
                std::copy_n((store ? source : initial).begin(), width, expected.begin());
                if (store)
                {
                    EXPECT_EQ(emu->read_memory<vector>(data), expected);
                }
                else
                {
                    EXPECT_EQ(emu->reg<vector>(zmm(9)), expected);
                }
            }
        }
        emu->reg(x86_register::r12, data);
        emu->write_memory(data, source.data(), source.size());
        emu->reg<vector>(zmm(9), pattern(8));
        load({0x66, 0x45, 0x0F, 0x6F, 0x0C, 0x24});
        emu->start(1);
        auto expected = pattern(8);
        std::copy_n(source.begin(), 16, expected.begin());
        EXPECT_EQ(emu->reg<vector>(zmm(9)), expected);
    }

    TEST_F(IcicleAlignedMove, ActiveOperandsReportPageFaultWithoutChangingDestination)
    {
        uint64_t reported{};
        memory_operation operation{};
        emu->hook_memory_violation([&](cpu_interface&, uint64_t address, size_t, memory_operation op, memory_violation_type) {
            reported = address;
            operation = op;
            return memory_violation_continuation::stop;
        });
        const auto inaccessible = memory->allocate_memory(0x1000, memory_permission::none);
        for (const auto pointer : {inaccessible, 0x100000000000ULL})
        {
            for (const bool store : {false, true})
            {
                for (const auto width : {16u, 32u, 64u})
                {
                    const auto initial = pattern(91);
                    emu->reg<vector>(zmm(20), initial);
                    emu->reg(x86_register::rax, pointer);
                    emu->reg(x86_register::k4, 2ULL);
                    const auto address = load(evex(width, 4, 20, 0, 4, false, store, true));
                    EXPECT_THROW(emu->start(1), std::runtime_error);
                    EXPECT_EQ(reported, pointer + 4);
                    EXPECT_EQ(operation, store ? memory_operation::write : memory_operation::read);
                    EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address);
                    EXPECT_EQ(emu->reg<vector>(zmm(20)), initial);
                }
            }
        }
    }

    TEST_F(IcicleAlignedMove, MaskedElementsProduceNoMemoryAccessEvents)
    {
        emu->reg(x86_register::rax, data);
        emu->reg(x86_register::k5, 0xA5ULL);
        emu->reg<vector>(zmm(30), pattern(32));
        for (const bool store : {false, true})
        {
            std::array<bool, 64> touched{};
            const auto callback = [&](cpu_interface&, uint64_t address, const void*, size_t size) {
                for (size_t i = 0; i < size; ++i)
                {
                    touched.at(address - data + i) = true;
                }
            };
            auto* const hook = store ? emu->hook_memory_write(data, 64, callback) : emu->hook_memory_read(data, 64, callback);
            load(evex(64, 8, 30, 0, 5, false, store, true));
            emu->start(1);
            emu->delete_hook(hook);
            for (size_t i = 0; i < 64; ++i)
            {
                EXPECT_EQ(touched[i], bool((0xA5 >> (i / 8)) & 1));
            }
        }
    }

    TEST_F(IcicleAlignedMove, RipRelativeAndAddressOverrideWithFsBase)
    {
        const auto source = pattern(2);
        const auto instruction = code + cursor;
        const auto destination = instruction + 0x100;
        const auto delta = static_cast<uint32_t>(destination - (instruction + 8));
        emu->write_memory(destination, source.data(), 16);
        load({0x66, 0x0F, 0x6F, 0x0D, static_cast<uint8_t>(delta), static_cast<uint8_t>(delta >> 8), 0, 0});
        emu->start(1);
        auto expected = vector{};
        std::copy_n(source.begin(), 16, expected.begin());
        EXPECT_EQ(emu->reg<vector>(zmm(1)), expected);
        emu->reg(x86_register::fs_base, data);
        emu->reg(x86_register::rax, 0x123400000040ULL);
        emu->write_memory(data + 0x40, source.data(), source.size());
        auto bytes = evex(64, 8, 28, 0, 0, false, false, true);
        bytes.insert(bytes.begin(), {0x64, 0x67});
        const auto address = load(bytes);
        emu->start(1);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + bytes.size());
        EXPECT_EQ(emu->reg<vector>(zmm(28)), source);
    }

    TEST_F(IcicleAlignedMove, RepeatedStoreThroughput)
    {
        using clock = std::chrono::steady_clock;
        emu->reg(x86_register::rax, data);
        emu->reg(x86_register::rcx, 100000ULL);
        const std::array<uint64_t, 2> source{0x1234567890ABCDEF, 0xFEDCBA0987654321};
        emu->reg<std::array<uint64_t, 2>>(x86_register::xmm1, source);
        const auto address = load({0x66, 0x0F, 0x7F, 0x08, 0x48, 0xFF, 0xC9, 0x75, 0xF7});
        const auto started = clock::now();
        emu->start(300000);
        const auto elapsed = std::chrono::duration<double>(clock::now() - started).count();
        RecordProperty("store_count", 100000);
        RecordProperty("seconds", std::to_string(elapsed));
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + 9);
        EXPECT_EQ((emu->read_memory<std::array<uint64_t, 2>>(data)), source);
    }

    TEST_F(IcicleAlignedMove, MixedInstructionBlockSeesUpdatedVectorState)
    {
        const std::array<uint64_t, 2> input{0x1122334455667788, 0x8877665544332211};
        emu->write_memory(data, input.data(), sizeof(input));
        emu->reg(x86_register::rbx, data);
        const auto address = load({0x66, 0x0F, 0xEF, 0xC9, 0x66, 0x0F, 0x6F, 0x0B, 0x66, 0x48, 0x0F, 0x7E, 0xC8});
        emu->start(3);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rax), input[0]);
        EXPECT_EQ(emu->reg<uint64_t>(x86_register::rip), address + 13);
    }

}
