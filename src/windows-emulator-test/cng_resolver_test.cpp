#include "emulation_test_utils.hpp"
#include <devices/cng_resolver.hpp>
#include <devices/security_support_provider.hpp>

namespace sogen::test
{
    class CngResolverTest : public testing::Test
    {
      protected:
        windows_emulator emu{create_empty_emulator()};
        const std::u16string root = u"\\registry\\machine\\system\\CurrentControlSet\\Control\\Cryptography\\";

        void value(const std::u16string& path, const std::string& name, const uint32_t type, const std::span<const std::byte> bytes)
        {
            const auto key = emu.registry.create_key(root + path);
            ASSERT_TRUE(key);
            emu.registry.set_value(*key, name, type, bytes);
        }

        void flags(const std::u16string& path, const uint32_t bits)
        {
            value(path, "Flags", REG_DWORD, std::as_bytes(std::span{&bits, 1}));
        }

        void strings(const std::u16string& path, const std::string& name, const std::initializer_list<std::u16string_view> values)
        {
            std::u16string data;
            for (const auto item : values)
            {
                data.append(item);
                data.push_back(0);
            }
            data.push_back(0);
            value(path, name, REG_MULTI_SZ, std::as_bytes(std::span{data}));
        }

        void add_provider(const std::u16string& name, const bool kernel = false)
        {
            const auto path = u"Providers\\" + name + (kernel ? u"\\KM" : u"\\UM");
            const std::u16string image = kernel ? u"example.sys\0"s : u"example.dll\0"s;
            value(path, "Image", REG_SZ, std::as_bytes(std::span{image}));
            flags(path + u"\\00000001", 1);
            strings(path + u"\\00000001", "Functions", {u"AES", u"3DES"});
        }

        static cng::resolution_request request()
        {
            return {.function = u"AES", .mode = 1};
        }

        cng::resolution_result resolve(const cng::resolution_request& input)
        {
            return cng::resolve(emu.registry, input);
        }
    };

    TEST_F(CngResolverTest, RealGuestCatalogResolvesCipherAndProperties)
    {
        const auto result = resolve(request());
        ASSERT_EQ(result.status, STATUS_SUCCESS);
        ASSERT_EQ(result.providers.size(), 1u);
        const auto& provider = result.providers.front();
        EXPECT_EQ(provider.interface_id, 1u);
        EXPECT_EQ(provider.function, u"AES");
        EXPECT_EQ(provider.provider, u"Microsoft Primitive Provider");
        ASSERT_TRUE(provider.user_image);
        EXPECT_EQ(provider.user_image->name, u"bcryptprimitives.dll");
        EXPECT_FALSE(provider.kernel_image);
        const auto property = std::ranges::find(provider.properties, u"KeyLength", &cng::property_reference::name);
        ASSERT_NE(property, provider.properties.end());
        EXPECT_EQ(property->value, (std::vector<uint8_t>{128, 0, 0, 0}));
    }

    TEST_F(CngResolverTest, ResolvesHashRngAndLongKdfNamesFromCatalog)
    {
        for (const auto& [name, id] : std::array<std::pair<std::u16string_view, uint32_t>, 4>{
                 {{u"SHA256", 2}, {u"MD5", 2}, {u"RNG", 6}, {u"SP800_108_CTR_HMAC", 7}}})
        {
            auto input = request();
            input.function = name;
            const auto result = resolve(input);
            ASSERT_EQ(result.status, STATUS_SUCCESS);
            ASSERT_EQ(result.providers.size(), 1u);
            EXPECT_EQ(result.providers.front().interface_id, id);
            EXPECT_EQ(result.providers.front().function, name);
        }
    }

    TEST_F(CngResolverTest, NamesAreCaseInsensitiveAndUnknownsDoNotBecomeRng)
    {
        auto input = request();
        input.function = u"aEs";
        EXPECT_EQ(resolve(input).status, STATUS_SUCCESS);
        input.function = u"MissingAlgorithm";
        EXPECT_EQ(resolve(input).status, STATUS_NOT_FOUND);
        input = request();
        input.provider = u"MissingProvider";
        EXPECT_EQ(resolve(input).status, STATUS_NOT_FOUND);
        input.provider = u"microsoft primitive provider";
        EXPECT_EQ(resolve(input).status, STATUS_SUCCESS);
        input.context = u"MissingContext";
        EXPECT_EQ(resolve(input).status, STATUS_NOT_FOUND);
    }

    TEST_F(CngResolverTest, ValidatesModeInterfaceFlagsAndAmbiguousAlgorithms)
    {
        for (const auto mode : {0u, 4u, UINT32_MAX})
        {
            auto input = request();
            input.mode = mode;
            EXPECT_EQ(resolve(input).status, STATUS_INVALID_PARAMETER);
        }
        auto input = request();
        input.flags = 4;
        EXPECT_EQ(resolve(input).status, STATUS_INVALID_PARAMETER);
        input = request();
        input.interface_id = 8;
        EXPECT_EQ(resolve(input).status, STATUS_INVALID_PARAMETER);
        input.interface_id = 2;
        EXPECT_EQ(resolve(input).status, STATUS_NOT_FOUND);
        input = request();
        input.function.reset();
        EXPECT_EQ(resolve(input).status, STATUS_INVALID_PARAMETER);
        strings(u"Providers\\Ambiguous\\UM\\00000002", "Functions", {u"AES"});
        EXPECT_EQ(resolve(request()).status, STATUS_DATA_ERROR);
    }

    TEST_F(CngResolverTest, ExplicitProviderWithoutFunctionProducesAnInterfaceReference)
    {
        auto input = request();
        input.interface_id = 1;
        input.function.reset();
        input.provider = u"Microsoft Primitive Provider";
        input.flags = 3;
        const auto result = resolve(input);
        ASSERT_EQ(result.status, STATUS_SUCCESS);
        ASSERT_EQ(result.providers.size(), 1u);
        EXPECT_FALSE(result.providers.front().function);
        EXPECT_TRUE(result.providers.front().properties.empty());
    }

    TEST_F(CngResolverTest, BothModesRequireBothImagesInThisWindowsBuild)
    {
        auto input = request();
        input.mode = 3;
        const auto result = resolve(input);
        ASSERT_EQ(result.status, STATUS_SUCCESS);
        EXPECT_TRUE(result.providers.front().user_image);
        EXPECT_TRUE(result.providers.front().kernel_image);
        add_provider(u"Test UM Only");
        input.provider = u"Test UM Only";
        EXPECT_EQ(resolve(input).status, STATUS_NOT_FOUND);
        add_provider(u"Test UM Only", true);
        EXPECT_EQ(resolve(input).status, STATUS_SUCCESS);
    }

    TEST_F(CngResolverTest, AllFunctionsAndAllProvidersAreIndependent)
    {
        auto input = request();
        input.interface_id = 1;
        input.function.reset();
        auto first = resolve(input);
        ASSERT_EQ(first.status, STATUS_SUCCESS);
        ASSERT_EQ(first.providers.size(), 1u);
        input.flags = 1;
        auto all = resolve(input);
        ASSERT_EQ(all.status, STATUS_SUCCESS);
        EXPECT_GE(all.providers.size(), 8u);
        add_provider(u"Extra Cipher");
        input = request();
        input.flags = 2;
        all = resolve(input);
        ASSERT_EQ(all.status, STATUS_SUCCESS);
        EXPECT_GE(all.providers.size(), 2u);
        EXPECT_TRUE(std::ranges::any_of(all.providers, [](const auto& provider) { return provider.provider == u"Extra Cipher"; }));
    }

    TEST_F(CngResolverTest, ContextPriorityFallbackIsolationAndPropertyPrecedence)
    {
        add_provider(u"Context Cipher");
        const std::u16string context = u"Configuration\\Local\\Audit";
        flags(context, 0);
        strings(context + u"\\00000001", "Functions", {u"AES"});
        strings(context + u"\\00000001\\AES", "Providers", {u"Context Cipher", u"Context Cipher"});
        const uint32_t key_length = 256;
        value(context + u"\\00000001\\AES\\Properties", "KeyLength", REG_BINARY, std::as_bytes(std::span{&key_length, 1}));
        auto input = request();
        input.context = u"Audit";
        input.flags = 2;
        auto result = resolve(input);
        ASSERT_EQ(result.status, STATUS_SUCCESS);
        EXPECT_EQ(result.providers.front().provider, u"Context Cipher");
        EXPECT_EQ(result.providers.front().properties.front().value, (std::vector<uint8_t>{0, 1, 0, 0}));
        EXPECT_EQ(std::ranges::count(result.providers, u"Context Cipher", &cng::provider_reference::provider), 1);
        EXPECT_GT(result.providers.size(), 1u);
        flags(context + u"\\00000001\\AES", 1);
        result = resolve(input);
        ASSERT_EQ(result.status, STATUS_SUCCESS);
        EXPECT_EQ(result.providers.size(), 1u);
        flags(context, 1);
        strings(context + u"\\00000001\\AES", "Providers", {u"Missing"});
        EXPECT_EQ(resolve(input).status, STATUS_NOT_FOUND);
    }

    TEST_F(CngResolverTest, DomainOverridePrecedesLocalDefault)
    {
        add_provider(u"Domain Cipher");
        const std::u16string domain = u"Configuration\\Domain\\Default";
        strings(domain + u"\\00000001", "Functions", {u"AES"});
        strings(domain + u"\\00000001\\AES", "Providers", {u"Domain Cipher"});
        EXPECT_EQ(resolve(request()).providers.front().provider, u"Microsoft Primitive Provider");
        flags(domain, 0x10000);
        const auto result = resolve(request());
        ASSERT_EQ(result.status, STATUS_SUCCESS);
        EXPECT_EQ(result.providers.front().provider, u"Domain Cipher");
    }

    class CngDeviceTest : public CngResolverTest
    {
      protected:
        std::unique_ptr<io_device> device{create_security_support_provider({})};
        uint64_t memory{};

        void SetUp() override
        {
            memory = emu.memory.allocate_memory(0x8000, memory_permission::read_write);
        }

        io_device_context context(const uint32_t input_size = 56, const uint32_t output_size = 4096)
        {
            io_device_context c{emu.memory};
            c.io_control_code = 0x390400;
            c.input_buffer = memory + 0x100;
            c.input_buffer_length = input_size;
            c.output_buffer = memory + 0x1000;
            c.output_buffer_length = output_size;
            c.io_status_block = {emu.memory, memory};
            return c;
        }

        void aes()
        {
            const std::array<uint64_t, 7> input{0x200001a2b3c4dULL, UINT64_MAX, 0, 48, UINT64_MAX, 1, 0x5300450041};
            emu.emu().write_memory(memory + 0x100, input);
        }

        uint64_t information()
        {
            return emu.memory.read_memory<IO_STATUS_BLOCK<EmulatorTraits<Emu64>>>(memory).Information;
        }

        uint32_t result_status()
        {
            return emu.memory.read_memory<uint32_t>(memory + 0x1000);
        }

        NTSTATUS notification(const handle event, const bool remove = false)
        {
            std::array<uint64_t, 13> input{};
            input[0] =
                (static_cast<uint64_t>(remove ? cng::unregister_notification : cng::register_notification) << 32) | cng::transport_magic;
            for (const auto index : {2u, 4u, 5u, 6u, 8u, 9u, 11u})
            {
                input[index] = UINT64_MAX;
            }
            input[12] = event.bits;
            emu.emu().write_memory(memory + 0x100, input);
            return device->execute_ioctl(emu, context(sizeof(input)));
        }
    };

    TEST_F(CngDeviceTest, ReportsRequiredSizeAndClearsUnusedOutput)
    {
        aes();
        ASSERT_EQ(device->execute_ioctl(emu, context(56, 8)), STATUS_BUFFER_OVERFLOW);
        ASSERT_EQ(result_status(), static_cast<uint32_t>(STATUS_BUFFER_TOO_SMALL));
        EXPECT_EQ(information(), 8u);
        const auto required = emu.memory.read_memory<uint32_t>(memory + 0x1004);
        ASSERT_GT(required, 216u);
        emu.memory.set_memory(memory + 0x1000, 0xa5, 4096);
        ASSERT_EQ(device->execute_ioctl(emu, context()), STATUS_SUCCESS);
        EXPECT_EQ(information(), required);
        EXPECT_EQ(result_status(), 1u);
        for (uint32_t i = required; i < 4096; ++i)
        {
            EXPECT_EQ(emu.memory.read_memory<uint8_t>(memory + 0x1000 + i), 0u);
        }
        ASSERT_EQ(device->execute_ioctl(emu, context(56, required)), STATUS_SUCCESS);
        EXPECT_EQ(information(), required);
    }

    TEST_F(CngDeviceTest, PreservesUnderlyingFailuresAndRejectsMalformedTransport)
    {
        aes();
        emu.emu().write_memory<uint32_t>(memory + 0x110, 2);
        ASSERT_EQ(device->execute_ioctl(emu, context()), STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(result_status(), static_cast<uint32_t>(STATUS_NOT_FOUND));
        EXPECT_EQ(information(), 4u);
        aes();
        emu.emu().write_memory<uint64_t>(memory + 0x118, UINT64_MAX - 1);
        ASSERT_EQ(device->execute_ioctl(emu, context()), STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(result_status(), static_cast<uint32_t>(STATUS_INTERNAL_ERROR));
        aes();
        emu.emu().write_memory<uint32_t>(memory + 0x100, 0);
        EXPECT_EQ(device->execute_ioctl(emu, context()), static_cast<NTSTATUS>(0xc00000af));
        EXPECT_EQ(information(), 0u);
        aes();
        emu.emu().write_memory<uint32_t>(memory + 0x104, 0x29999);
        EXPECT_EQ(device->execute_ioctl(emu, context()), static_cast<NTSTATUS>(0xc00000af));
    }

    TEST_F(CngDeviceTest, HandlesShortOutputAndInvalidGuestMemory)
    {
        aes();
        emu.memory.set_memory(memory + 0x1000, 0xa5, 8);
        EXPECT_EQ(device->execute_ioctl(emu, context(56, 7)), STATUS_BUFFER_TOO_SMALL);
        EXPECT_EQ(information(), 0u);
        EXPECT_EQ(emu.memory.read_memory<uint64_t>(memory + 0x1000), 0xa500000000000000ULL);
        auto c = context();
        c.input_buffer = 1;
        EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_ACCESS_VIOLATION);
        c = context();
        c.output_buffer = 1;
        EXPECT_EQ(device->execute_ioctl(emu, c), STATUS_ACCESS_VIOLATION);
    }

    TEST_F(CngDeviceTest, Wow64UsesTheSameWireRecords)
    {
        aes();
        ASSERT_EQ(device->execute_ioctl(emu, context()), STATUS_SUCCESS);
        std::vector<uint8_t> expected(information());
        emu.memory.read_memory(memory + 0x1000, expected.data(), expected.size());
        device = create_security_support_provider({.is_32_bit = true});
        ASSERT_EQ(device->execute_ioctl(emu, context()), STATUS_SUCCESS);
        std::vector<uint8_t> actual(information());
        emu.memory.read_memory(memory + 0x1000, actual.data(), actual.size());
        EXPECT_EQ(expected, actual);
    }

    TEST_F(CngDeviceTest, NotificationRetainsEventAndHandlesDuplicateCancellation)
    {
        const auto event = emu.process.events.store(sogen::event{});
        ASSERT_EQ(notification(event), STATUS_SUCCESS);
        EXPECT_EQ(information(), 0u);
        EXPECT_EQ(emu.process.events.get(event)->ref_count, 2u);
        EXPECT_FALSE(emu.process.events.get(event)->signaled);
        EXPECT_EQ(notification(event), STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(result_status(), static_cast<uint32_t>(STATUS_OBJECT_NAME_COLLISION));
        EXPECT_EQ(emu.process.events.get(event)->ref_count, 2u);
        emu.cng_changes.publish(emu);
        EXPECT_TRUE(emu.process.events.get(event)->signaled);
        ASSERT_EQ(notification(event, true), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.events.get(event)->ref_count, 1u);
        EXPECT_EQ(notification(event, true), STATUS_SUCCESS);
        emu.process.events.get(event)->signaled = false;
        emu.cng_changes.publish(emu);
        EXPECT_FALSE(emu.process.events.get(event)->signaled);
        EXPECT_EQ(notification(make_handle(0)), STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(result_status(), static_cast<uint32_t>(STATUS_INVALID_HANDLE));
        const auto other = emu.process.mutants.store(mutant{});
        EXPECT_EQ(notification(other), STATUS_BUFFER_OVERFLOW);
        EXPECT_EQ(result_status(), static_cast<uint32_t>(STATUS_OBJECT_TYPE_MISMATCH));
    }

    TEST_F(CngDeviceTest, NotificationSurvivesHandleCloseAndSnapshotRestore)
    {
        const auto event = emu.process.events.store(sogen::event{});
        ASSERT_EQ(notification(event), STATUS_SUCCESS);
        emu.process.events.erase(event);
        ASSERT_NE(emu.process.events.get(event), nullptr);
        utils::buffer_serializer saved;
        emu.serialize(saved);
        ASSERT_EQ(emu.cng_changes.unsubscribe(emu, event), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.events.get(event), nullptr);
        utils::buffer_deserializer input{saved.get_buffer()};
        emu.deserialize(input);
        EXPECT_EQ(input.get_remaining_size(), 0u);
        ASSERT_NE(emu.process.events.get(event), nullptr);
        emu.cng_changes.publish(emu);
        EXPECT_TRUE(emu.process.events.get(event)->signaled);
        EXPECT_EQ(emu.cng_changes.unsubscribe(emu, event), STATUS_SUCCESS);
        EXPECT_EQ(emu.process.events.get(event), nullptr);
    }

    TEST_F(CngDeviceTest, ReadsLegacySnapshotWithoutNotificationTrailer)
    {
        utils::buffer_serializer saved;
        utils::buffer_serializer extension;
        emu.serialize(saved);
        emu.cng_changes.serialize(extension);
        auto bytes = saved.move_buffer();
        bytes.resize(bytes.size() - extension.get_buffer().size());
        utils::buffer_deserializer input{bytes};
        emu.deserialize(input);
        EXPECT_EQ(input.get_remaining_size(), 0u);
    }
}
