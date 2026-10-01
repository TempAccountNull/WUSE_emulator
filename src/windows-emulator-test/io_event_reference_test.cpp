#include "emulation_test_utils.hpp"
#include <io_device.hpp>
#include <syscall_utils.hpp>
#include <array>

namespace sogen::syscalls
{
    NTSTATUS handle_NtClose(const syscall_context&, handle);
    NTSTATUS handle_NtPulseEvent(const syscall_context&, uint64_t, emulator_object<LONG>);
    NTSTATUS handle_NtDeviceIoControlFile(const syscall_context&, handle, handle, emulator_pointer, emulator_pointer,
                                          emulator_object<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>, ULONG, emulator_pointer, ULONG,
                                          emulator_pointer, ULONG);
}

namespace sogen::test
{
    namespace
    {
        struct deferred_event_device : stateless_device
        {
            std::optional<io_device_context> pending{};
            bool ready{};
            NTSTATUS terminal_status{STATUS_SUCCESS};

            bool may_return_pending() const override
            {
                return true;
            }

            NTSTATUS io_control(windows_emulator&, const io_device_context& request) override
            {
                if (!this->ready)
                {
                    if (!this->pending)
                    {
                        this->pending.emplace(request);
                    }
                    return STATUS_PENDING;
                }
                request.io_status_block.access([&](auto& block) { block.Information = this->terminal_status == STATUS_SUCCESS ? 3 : 0; });
                return this->terminal_status;
            }

            NTSTATUS finish(windows_emulator& emu, const NTSTATUS status)
            {
                auto request = *this->pending;
                request.completing_pending = true;
                this->ready = true;
                this->terminal_status = status;
                const auto result = this->execute_ioctl(emu, request);
                this->pending.reset();
                return result;
            }
        };
    }

    class IoEventReferenceTest : public testing::Test
    {
      protected:
        windows_emulator emu = [] {
            emulator_settings settings{.disable_logging = true};
            settings.path_mappings[R"(C:\test-sample.exe)"] = std::filesystem::current_path() / "test-sample.exe";
            return create_sample_emulator(std::move(settings));
        }();
        uint64_t memory{};

        void SetUp() override
        {
            emu.setup_process_if_necessary();
            memory = emu.memory.allocate_memory(0x1000, memory_permission::read_write);
            ASSERT_NE(memory, 0u);
        }

        syscall_context context()
        {
            auto& vcpu = emu.vcpu(0);
            return {.win_emu = emu, .emu = vcpu.cpu, .vcpu = vcpu, .proc = emu.process};
        }

        io_device_context request(const handle event_handle)
        {
            io_device_context result{emu.memory};
            result.event = event_handle;
            result.io_status_block = {emu.memory, memory};
            result.io_status_block.write({.Pointer = 0xaabbccdd4a4b4c4dULL, .Information = 0x1122334455667788ULL});
            return result;
        }

        void factories(utils::buffer_deserializer& input)
        {
            input.register_factory<x64_emulator_wrapper>([this] { return x64_emulator_wrapper{emu.emu()}; });
            input.register_factory<windows_emulator_wrapper>([this] { return windows_emulator_wrapper{emu}; });
        }
    };

    TEST_F(IoEventReferenceTest, CanonicalHandlesReserveBothBitsAndAliasesSelectOneObject)
    {
        const auto base = emu.process.events.store(event{});
        const auto decoy = emu.process.events.store(event{});
        ASSERT_EQ(base.bits & 3u, 0u);
        ASSERT_EQ(decoy.bits & 3u, 0u);
        ASSERT_NE(base.bits, decoy.bits);
        auto* selected = emu.process.events.get(base);
        ASSERT_NE(selected, nullptr);
        for (uint64_t tag = 0; tag < 4; ++tag)
        {
            const auto alias = make_handle(base.bits | tag);
            ASSERT_TRUE(emu.process.events.contains(alias));
            ASSERT_EQ(emu.process.events.get(alias), selected);
            selected->signaled = false;
            emu.process.events.get(alias)->signaled = true;
            EXPECT_TRUE(emu.process.events.get(base)->signaled);
            EXPECT_FALSE(emu.process.events.get(decoy)->signaled);
            EXPECT_EQ(emu.process.events.find_handle(selected), base);
        }
        EXPECT_EQ(emu.process.events.get(NULL_HANDLE), nullptr);
        EXPECT_EQ(emu.process.events.get(WER_PORT_READY), nullptr);
        EXPECT_EQ(emu.process.events.get(make_handle(static_cast<uint32_t>(base.value.id), handle_types::file, false)), nullptr);
        EXPECT_EQ(emu.process.events.get(emu.process.events.make_handle(0)), nullptr);
        EXPECT_EQ(emu.process.events.get_by_index(decltype(emu.process.events)::maximum_index + 1), nullptr);
    }

    TEST_F(IoEventReferenceTest, RepresentableStoreCapacityCannotWrapIntoAnotherHandle)
    {
        handle_store<handle_types::event, event, 21> tiny{};
        static_assert(decltype(tiny)::maximum_index == 3);
        static_assert(decltype(emu.process.events)::maximum_index == 2097151);
        const auto first = tiny.store(event{});
        const auto second = tiny.store(event{});
        const auto third = tiny.store(event{});
        EXPECT_NE(first.bits, second.bits);
        EXPECT_NE(second.bits, third.bits);
        EXPECT_EQ(third.value.id, 3u << 21);
        EXPECT_THROW(tiny.store(event{}), std::length_error);
        EXPECT_THROW(tiny.make_handle(4), std::out_of_range);
        EXPECT_EQ(tiny.get_by_index(4), nullptr);
        ASSERT_TRUE(tiny.erase(second));
        EXPECT_EQ(tiny.store(event{}), second);
        decltype(tiny)::value_map invalid_entries{};
        invalid_entries.emplace(4, event{});
        utils::buffer_serializer invalid_saved{};
        invalid_saved.write(false);
        invalid_saved.write_map(invalid_entries);
        utils::buffer_deserializer invalid_input{invalid_saved};
        EXPECT_THROW(tiny.deserialize(invalid_input), std::runtime_error);
    }

    TEST_F(IoEventReferenceTest, ContextCopiesShareOneHoldAndSeparateDispatchesAcquireTheirOwn)
    {
        const auto base = emu.process.events.store(event{});
        auto original = request(make_handle(base.bits | 1u));
        original.capture_event_reference(emu.process);
        ASSERT_EQ(emu.process.events.get(base)->ref_count, 2u);
        {
            auto transient = original;
            EXPECT_EQ(transient.event_reference, original.event_reference);
            EXPECT_EQ(emu.process.events.get(base)->ref_count, 2u);
        }
        deferred_event_device first{};
        deferred_event_device second{};
        ASSERT_EQ(first.execute_ioctl(emu, original), STATUS_PENDING);
        ASSERT_EQ(second.execute_ioctl(emu, original), STATUS_PENDING);
        ASSERT_EQ(emu.process.events.get(base)->ref_count, 4u);
        ASSERT_NE(first.pending->event_reference, second.pending->event_reference);
        original.event_reference.reset();
        EXPECT_EQ(emu.process.events.get(base)->ref_count, 3u);
        EXPECT_EQ(first.finish(emu, STATUS_SUCCESS), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.events.get(base)->ref_count, 2u);
        EXPECT_EQ(second.finish(emu, STATUS_CANCELLED), STATUS_CANCELLED);
        EXPECT_EQ(emu.process.events.get(base)->ref_count, 1u);
    }

    TEST_F(IoEventReferenceTest, PendingCloseRetainsSelectedObjectAndPreventsPrematureRecycling)
    {
        const auto base = emu.process.events.store(event{});
        const auto decoy = emu.process.events.store(event{});
        auto* selected = emu.process.events.get(base);
        deferred_event_device device{};
        auto submitted = request(make_handle(base.bits | 3u));
        const auto iosb_before = emu.memory.read_memory(memory, 16);
        ASSERT_EQ(device.execute_ioctl(emu, submitted), STATUS_PENDING);
        ASSERT_EQ(emu.memory.read_memory(memory, 16), iosb_before);
        ASSERT_EQ(emu.process.events.get(base)->ref_count, 2u);
        ASSERT_EQ(syscalls::handle_NtClose(context(), base), STATUS_SUCCESS);
        ASSERT_EQ(emu.process.events.get(base), selected);
        ASSERT_EQ(selected->ref_count, 1u);
        const auto later = emu.process.events.store(event{});
        ASSERT_NE(later.bits, base.bits);
        auto retry = *device.pending;
        retry.completing_pending = true;
        ASSERT_EQ(device.execute_ioctl(emu, retry), STATUS_PENDING);
        EXPECT_EQ(selected->ref_count, 1u);
        EXPECT_FALSE(selected->signaled);
        EXPECT_FALSE(emu.process.events.get(decoy)->signaled);
        ASSERT_EQ(device.finish(emu, STATUS_SUCCESS), STATUS_SUCCESS);
        EXPECT_TRUE(selected->signaled);
        EXPECT_FALSE(emu.process.events.get(later)->signaled);
        EXPECT_FALSE(emu.process.events.get(decoy)->signaled);
        retry.event_reference.reset();
        EXPECT_EQ(emu.process.events.get(base), nullptr);
        const auto recycled = emu.process.events.store(event{});
        EXPECT_EQ(recycled.bits, base.bits);
        EXPECT_FALSE(emu.process.events.get(recycled)->signaled);
    }

    TEST_F(IoEventReferenceTest, TerminalStatusesAndDroppedPendingContextsReleaseTheirHold)
    {
        for (const auto status : std::array{static_cast<NTSTATUS>(STATUS_SUCCESS), static_cast<NTSTATUS>(STATUS_CANCELLED),
                                            static_cast<NTSTATUS>(STATUS_TIMEOUT)})
        {
            const auto base = emu.process.events.store(event{});
            deferred_event_device device{};
            auto submitted = request(make_handle(base.bits | 1u));
            ASSERT_EQ(device.execute_ioctl(emu, submitted), STATUS_PENDING);
            EXPECT_EQ(emu.process.events.get(base)->ref_count, 2u);
            ASSERT_EQ(device.finish(emu, status), status);
            EXPECT_TRUE(emu.process.events.get(base)->signaled);
            EXPECT_EQ(emu.process.events.get(base)->ref_count, 1u);
            EXPECT_EQ(submitted.io_status_block.read().Status, status);
            EXPECT_EQ(submitted.io_status_block.read().Information, status == STATUS_SUCCESS ? 3u : 0u);
        }
        const auto dropped = emu.process.events.store(event{});
        {
            deferred_event_device device{};
            ASSERT_EQ(device.execute_ioctl(emu, request(dropped)), STATUS_PENDING);
            EXPECT_EQ(emu.process.events.get(dropped)->ref_count, 2u);
        }
        EXPECT_EQ(emu.process.events.get(dropped)->ref_count, 1u);
        EXPECT_FALSE(emu.process.events.get(dropped)->signaled);
    }

    TEST_F(IoEventReferenceTest, FailedCaptureCannotSelectAnObjectCreatedLater)
    {
        const auto invalid = emu.process.events.store(event{});
        ASSERT_TRUE(emu.process.events.erase(invalid));
        ASSERT_EQ(emu.process.events.get(invalid), nullptr);
        auto submitted = request(invalid);
        submitted.capture_event_reference(emu.process);
        ASSERT_TRUE(submitted.event_reference_captured);
        ASSERT_EQ(submitted.event_reference, nullptr);
        const auto later = emu.process.events.store(event{});
        ASSERT_EQ(later.bits, invalid.bits);
        submitted.capture_event_reference(emu.process, true);
        EXPECT_EQ(submitted.completion_event(emu.process), nullptr);
        complete_device_ioctl(emu, submitted, STATUS_SUCCESS, true);
        EXPECT_FALSE(emu.process.events.get(later)->signaled);
    }

    TEST_F(IoEventReferenceTest, SyscallResetsResolvedAliasBeforeDispatchAndSignalsItAtCompletion)
    {
        const auto base = emu.process.events.store(event{});
        const auto decoy = emu.process.events.store(event{});
        const auto device = emu.process.devices.store(io_device_container{u"CNG", emu, {}});
        for (uint64_t tag = 0; tag < 4; ++tag)
        {
            auto submitted = request(make_handle(base.bits | tag));
            emu.process.events.get(base)->signaled = true;
            emu.process.events.get(decoy)->signaled = true;
            bool observed_dispatch{};
            emu.callbacks.on_ioctrl = [&](io_device&, std::u16string_view, ULONG) {
                observed_dispatch = true;
                EXPECT_FALSE(emu.process.events.get(base)->signaled);
                EXPECT_TRUE(emu.process.events.get(decoy)->signaled);
                EXPECT_EQ(emu.process.events.get(base)->ref_count, 3u);
            };
            ASSERT_EQ(
                syscalls::handle_NtDeviceIoControlFile(context(), device, submitted.event, 0, 0, submitted.io_status_block, 0, 0, 0, 0, 0),
                STATUS_SUCCESS);
            EXPECT_TRUE(observed_dispatch);
            EXPECT_TRUE(emu.process.events.get(base)->signaled);
            EXPECT_TRUE(emu.process.events.get(decoy)->signaled);
            EXPECT_EQ(emu.process.events.get(base)->ref_count, 1u);
            EXPECT_EQ(submitted.io_status_block.read().Status, STATUS_SUCCESS);
        }
        emu.callbacks.on_ioctrl = {};
    }

    TEST_F(IoEventReferenceTest, NullPseudoAndWrongTypeCaptureRemainUnavailableWithoutSelectingAnEvent)
    {
        const auto base = emu.process.events.store(event{});
        const auto wrong_type = make_handle(static_cast<uint32_t>(base.value.id), handle_types::file, false);
        for (const auto supplied : {NULL_HANDLE, WER_PORT_READY, wrong_type})
        {
            auto submitted = request(supplied);
            submitted.capture_event_reference(emu.process);
            ASSERT_TRUE(submitted.event_reference_captured);
            EXPECT_EQ(submitted.event_reference, nullptr);
            EXPECT_EQ(submitted.completion_event(emu.process), nullptr);
            utils::buffer_serializer saved{};
            saved.write(submitted);
            utils::buffer_deserializer input{saved};
            factories(input);
            auto restored = input.read<io_device_context>();
            EXPECT_TRUE(restored.event_reference_captured);
            EXPECT_EQ(restored.event_reference, nullptr);
            complete_device_ioctl(emu, restored, STATUS_SUCCESS, false);
            EXPECT_FALSE(emu.process.events.get(base)->signaled);
            EXPECT_EQ(emu.process.events.get(base)->ref_count, 1u);
        }
    }

    TEST_F(IoEventReferenceTest, SamePolicyEventStoreRoundTripPreservesCanonicalHandlesAndTagIdentity)
    {
        handle_store<handle_types::event, event, 2> original{};
        const auto base = original.store(event{});
        const auto decoy = original.store(event{});
        original.get(base)->signaled = true;
        utils::buffer_serializer saved{};
        saved.write(original);
        utils::buffer_deserializer input{saved};
        handle_store<handle_types::event, event, 2> restored{};
        input.read(restored);
        ASSERT_EQ(restored.size(), 2u);
        ASSERT_NE(restored.get(base), nullptr);
        EXPECT_EQ(restored.find_handle(restored.get(base)), base);
        EXPECT_EQ(restored.find_handle(restored.get(decoy)), decoy);
        EXPECT_TRUE(restored.get(make_handle(base.bits | 3u))->signaled);
        EXPECT_FALSE(restored.get(decoy)->signaled);
        EXPECT_EQ(restored.get(base)->ref_count, 1u);
    }

    TEST_F(IoEventReferenceTest, DeviceOnlyContextRoundTripAcquiresAnIndependentHold)
    {
        const auto base = emu.process.events.store(event{});
        auto original = request(make_handle(base.bits | 1u));
        original.capture_event_reference(emu.process);
        utils::buffer_serializer saved{};
        saved.write(original);
        utils::buffer_deserializer input{saved};
        factories(input);
        {
            auto restored = input.read<io_device_context>();
            EXPECT_EQ(restored.event.bits, original.event.bits);
            ASSERT_NE(restored.event_reference, nullptr);
            EXPECT_EQ(restored.event_reference->canonical_handle(), base);
            EXPECT_EQ(restored.completion_event(emu.process), original.completion_event(emu.process));
            EXPECT_EQ(emu.process.events.get(base)->ref_count, 3u);
        }
        EXPECT_EQ(emu.process.events.get(base)->ref_count, 2u);
    }

    TEST_F(IoEventReferenceTest, RestoredHoldAdoptsSavedCountAndOldEpochCannotTouchReplacement)
    {
        const auto base = emu.process.events.store(event{});
        auto original = request(make_handle(base.bits | 2u));
        original.capture_event_reference(emu.process);
        ASSERT_EQ(syscalls::handle_NtClose(context(), base), STATUS_SUCCESS);
        ASSERT_EQ(emu.process.events.get(base)->ref_count, 1u);
        utils::buffer_serializer saved_events{};
        emu.process.events.serialize(saved_events);
        utils::buffer_serializer saved_request{};
        saved_request.write(original);
        ++emu.process.io_event_reference_epoch;
        utils::buffer_deserializer event_input{saved_events};
        emu.process.events.deserialize(event_input);
        ASSERT_EQ(original.completion_event(emu.process), nullptr);
        EXPECT_THROW(original.serialize(saved_request), std::runtime_error);
        original.event_reference.reset();
        ASSERT_EQ(emu.process.events.get(base)->ref_count, 1u);
        utils::buffer_deserializer input{saved_request};
        factories(input);
        emu.process.adopting_restored_io_event_references = true;
        {
            auto restored = input.read<io_device_context>();
            emu.process.adopting_restored_io_event_references = false;
            EXPECT_EQ(emu.process.events.get(base)->ref_count, 1u);
            ASSERT_NE(restored.completion_event(emu.process), nullptr);
            complete_device_ioctl(emu, restored, STATUS_SUCCESS, false);
            EXPECT_TRUE(emu.process.events.get(base)->signaled);
        }
        EXPECT_EQ(emu.process.events.get(base), nullptr);
    }

    TEST_F(IoEventReferenceTest, LegacyProcessEncodingRejectsBeforeProcessFieldsChange)
    {
        const auto sid_before = emu.process.sid;
        const auto epoch_before = emu.process.io_event_reference_epoch;
        const auto events_before = emu.process.events.size();
        auto* active = emu.vcpu(0).active_thread;
        utils::buffer_serializer legacy{};
        legacy.write(uint64_t{});
        utils::buffer_deserializer input{legacy};
        EXPECT_THROW(emu.process.deserialize(input, active), std::runtime_error);
        EXPECT_EQ(emu.process.sid, sid_before);
        EXPECT_EQ(emu.process.io_event_reference_epoch, epoch_before);
        EXPECT_EQ(emu.process.events.size(), events_before);
        EXPECT_EQ(active, emu.vcpu(0).active_thread);
    }

    TEST_F(IoEventReferenceTest, TaggedPulseWakesAliasedWaiterUsingTheSameObject)
    {
        const auto base = emu.process.events.store(event{});
        const auto decoy = emu.process.events.store(event{});
        auto& thread = *emu.vcpu(0).active_thread;
        thread.await_objects = {make_handle(base.bits | 1u)};
        thread.await_any = true;
        EXPECT_FALSE(thread.is_thread_ready(emu));
        EXPECT_EQ(syscalls::handle_NtPulseEvent(context(), base.bits | 2u, {emu.memory, 0}), STATUS_SUCCESS);
        EXPECT_TRUE(thread.await_objects.empty());
        EXPECT_EQ(thread.pending_status, STATUS_WAIT_0);
        EXPECT_FALSE(emu.process.events.get(base)->signaled);
        EXPECT_FALSE(emu.process.events.get(decoy)->signaled);
    }
}
