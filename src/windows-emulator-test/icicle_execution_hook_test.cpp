#include "emulation_test_utils.hpp"
#include "../backends/icicle-emulator/execution_hook.hpp"
#include <memory_manager.hpp>
#include <array>
#include <vector>

namespace sogen::test
{
    class IcicleExecutionHooks : public testing::Test
    {
      protected:
        std::unique_ptr<x86_64_emulator> emu;
        std::unique_ptr<memory_manager> memory;
        uint64_t code{};

        void SetUp() override
        {
            emu = create_x86_64_emulator(backend_type::icicle);
            memory = std::make_unique<memory_manager>(*emu);
            code = memory->allocate_memory(0x1000, memory_permission::all);
            ASSERT_NE(code, 0U);
            const std::array<uint8_t, 5> bytes{0x90, 0x90, 0x90, 0x90, 0x90};
            emu->write_memory(code, bytes.data(), bytes.size());
            emu->reg(x86_register::rip, code);
        }
    };

    TEST_F(IcicleExecutionHooks, RegistrationFormsKeepCpuAndAddress)
    {
        std::vector<uint64_t> generic;
        std::vector<uint64_t> exact;
        std::vector<uint64_t> range;
        std::vector<uint64_t> single;
        size_t empty_range_calls{};
        emu->hook_memory_execution([&, payload = std::array<uint64_t, 16>{0x123456789abcdef0}](cpu_interface& cpu, const uint64_t address) {
            EXPECT_EQ(&cpu, static_cast<cpu_interface*>(emu.get()));
            EXPECT_EQ(payload[0], 0x123456789abcdef0U);
            generic.push_back(address);
        });
        emu->hook_memory_execution(code + 2, [&](cpu_interface& cpu, const uint64_t address) {
            EXPECT_EQ(&cpu, static_cast<cpu_interface*>(emu.get()));
            exact.push_back(address);
        });
        emu->hook_memory_range_execution(code + 1, 2, [&](cpu_interface& cpu, const uint64_t address) {
            EXPECT_EQ(&cpu, static_cast<cpu_interface*>(emu.get()));
            range.push_back(address);
        });
        emu->hook_memory_range_execution(code + 3, 1, [&](cpu_interface& cpu, const uint64_t address) {
            EXPECT_EQ(&cpu, static_cast<cpu_interface*>(emu.get()));
            single.push_back(address);
        });
        emu->hook_memory_range_execution(code, 0, [&](cpu_interface&, uint64_t) { ++empty_range_calls; });
        emu->start(4);
        EXPECT_EQ(generic, (std::vector<uint64_t>{code, code + 1, code + 2, code + 3}));
        EXPECT_EQ(exact, (std::vector<uint64_t>{code + 2}));
        EXPECT_EQ(range, (std::vector<uint64_t>{code + 1, code + 2}));
        EXPECT_EQ(single, (std::vector<uint64_t>{code + 3}));
        EXPECT_EQ(empty_range_calls, 0U);

        auto other = create_x86_64_emulator(backend_type::icicle);
        memory_manager other_memory(*other);
        const auto other_code = other_memory.allocate_memory(0x1000, memory_permission::all);
        other->write_memory<uint8_t>(other_code, 0x90);
        other->reg(x86_register::rip, other_code);
        size_t other_calls{};
        other->hook_memory_execution([&](cpu_interface& cpu, const uint64_t address) {
            EXPECT_EQ(&cpu, static_cast<cpu_interface*>(other.get()));
            EXPECT_NE(&cpu, static_cast<cpu_interface*>(emu.get()));
            EXPECT_EQ(address, other_code);
            ++other_calls;
        });
        other->start(1);
        EXPECT_EQ(other_calls, 1U);
        EXPECT_EQ(generic.size(), 4U);
    }

    TEST_F(IcicleExecutionHooks, SelfAndPeerRemovalKeepCallbacksAliveUntilDrain)
    {
        size_t destroyed{};
        auto lifetime = std::shared_ptr<int>(new int{}, [&](const int* value) {
            ++destroyed;
            delete value;
        });
        std::weak_ptr<int> weak = lifetime;
        emulator_hook* self{};
        emulator_hook* peer{};
        size_t self_calls{};
        size_t peer_calls{};
        peer = emu->hook_memory_execution(code + 2, [&, retained = lifetime](cpu_interface&, uint64_t) {
            EXPECT_NE(retained, nullptr);
            EXPECT_EQ(destroyed, 0U);
            ++peer_calls;
        });
        self = emu->hook_memory_execution(code + 1, [&, retained = lifetime](cpu_interface&, uint64_t) {
            ++self_calls;
            emu->delete_hook(self);
            emu->delete_hook(peer);
            EXPECT_NE(retained, nullptr);
            EXPECT_FALSE(weak.expired());
            EXPECT_EQ(destroyed, 0U);
        });
        lifetime.reset();
        emu->start(4);
        EXPECT_EQ(self_calls, 1U);
        EXPECT_EQ(peer_calls, 1U);
        EXPECT_TRUE(weak.expired());
        EXPECT_EQ(destroyed, 1U);
        emu->reg(x86_register::rip, code);
        emu->start(4);
        EXPECT_EQ(self_calls, 1U);
        EXPECT_EQ(peer_calls, 1U);
    }

    TEST_F(IcicleExecutionHooks, NestedCallbacksRestorePreviousState)
    {
        for (const auto initial : {false, true})
        {
            bool active = initial;
            bool deletion_would_be_deferred{};
            icicle::detail::execution_hook inner(
                *emu,
                [&](cpu_interface& cpu, const uint64_t address) {
                    EXPECT_TRUE(active);
                    EXPECT_EQ(&cpu, static_cast<cpu_interface*>(emu.get()));
                    EXPECT_EQ(address, code + 1);
                },
                active);
            icicle::detail::execution_hook outer(
                *emu,
                [&](cpu_interface&, uint64_t) {
                    EXPECT_TRUE(active);
                    inner(code + 1);
                    deletion_would_be_deferred = active;
                },
                active);
            outer(code);
            EXPECT_TRUE(deletion_would_be_deferred);
            EXPECT_EQ(active, initial);
        }
    }

    TEST_F(IcicleExecutionHooks, ExceptionsAndEmptyCallbacksRestoreState)
    {
        struct callback_error
        {
        };

        for (const auto initial : {false, true})
        {
            bool active = initial;
            icicle::detail::execution_hook throwing(
                *emu,
                [&](cpu_interface&, uint64_t) {
                    EXPECT_TRUE(active);
                    throw callback_error{};
                },
                active);
            EXPECT_THROW(throwing(code), callback_error);
            EXPECT_EQ(active, initial);
            icicle::detail::execution_hook empty(*emu, {}, active);
            EXPECT_THROW(empty(code), std::bad_function_call);
            EXPECT_EQ(active, initial);
            icicle::detail::execution_hook outer(
                *emu,
                [&](cpu_interface&, uint64_t) {
                    EXPECT_THROW(throwing(code), callback_error);
                    EXPECT_TRUE(active);
                },
                active);
            outer(code);
            EXPECT_EQ(active, initial);
        }
        auto* empty_registration = emu->hook_memory_execution(memory_execution_hook_callback{});
        EXPECT_NE(empty_registration, nullptr);
        emu->delete_hook(empty_registration);
    }
}
