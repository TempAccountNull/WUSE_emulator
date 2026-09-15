#include "emulation_test_utils.hpp"
#include <devices/cng_protocol.hpp>

namespace sogen::test
{
    namespace
    {
        const std::array<uint8_t, 56> captured_aes_request{
            0x4d, 0x3c, 0x2b, 0x1a, 0x00, 0x00, 0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x45, 0x00, 0x53, 0x00, 0x00, 0x00};

        template <typename T>
        T field(const std::vector<uint8_t>& bytes, const size_t offset)
        {
            if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
            {
                throw std::out_of_range("CNG response field");
            }
            T result{};
            memcpy(&result, bytes.data() + offset, sizeof(T));
            return result;
        }

        std::u16string wire_string(const std::vector<uint8_t>& bytes, const uint64_t offset)
        {
            std::u16string result;
            for (auto current = offset;; current += 2)
            {
                const auto character = field<char16_t>(bytes, current);
                if (!character)
                {
                    return result;
                }
                result.push_back(character);
            }
        }

        cng::provider_reference aes_provider()
        {
            return {.interface_id = 1,
                    .function = u"AES",
                    .provider = u"Microsoft Primitive Provider",
                    .properties = {{.name = u"KeyLength", .value = {0x80, 0, 0, 0}}},
                    .user_image = cng::image_reference{.name = u"bcryptprimitives.dll", .flags = 1},
                    .kernel_image = cng::image_reference{.name = u"cng.sys", .flags = 1}};
        }
    }

    TEST(CngProtocolTest, DecodesCapturedGuestAesRequest)
    {
        const auto request = cng::unpack_resolution(captured_aes_request);
        ASSERT_TRUE(request);
        EXPECT_FALSE(request->context);
        EXPECT_FALSE(request->provider);
        EXPECT_EQ(request->function, u"AES");
        EXPECT_EQ(request->interface_id, 0u);
        EXPECT_EQ(request->mode, 1u);
        EXPECT_EQ(request->flags, 0u);
    }

    TEST(CngProtocolTest, RejectsEveryTruncatedCapturedRequest)
    {
        for (size_t length = 0; length < captured_aes_request.size(); ++length)
        {
            if (length == 8)
            {
                continue;
            }
            EXPECT_FALSE(cng::unpack_resolution(std::span{captured_aes_request}.first(length))) << length;
        }
    }

    TEST(CngProtocolTest, RejectsWrongHeaderAndInvalidStringOffsets)
    {
        for (const uint64_t offset : {0ULL, 16ULL, 47ULL, 56ULL, 0x100000030ULL, UINT64_MAX - 1})
        {
            auto bytes = captured_aes_request;
            memcpy(bytes.data() + 24, &offset, sizeof(offset));
            EXPECT_FALSE(cng::unpack_resolution(bytes)) << offset;
        }
        for (const auto offset : {0u, 4u})
        {
            auto bytes = captured_aes_request;
            bytes[offset] ^= 1;
            EXPECT_FALSE(cng::unpack_resolution(bytes));
        }
    }

    TEST(CngProtocolTest, ReadsLongFunctionNamesAndIndependentStringOffsets)
    {
        std::vector<uint8_t> bytes(captured_aes_request.begin(), captured_aes_request.end());
        const auto append = [&](const size_t field_offset, const std::u16string& value) {
            const auto offset = static_cast<uint64_t>(bytes.size());
            const auto start = bytes.size();
            bytes.resize(start + (value.size() + 1) * 2);
            memcpy(bytes.data() + start, value.c_str(), (value.size() + 1) * 2);
            memcpy(bytes.data() + field_offset, &offset, sizeof(offset));
        };
        append(32, u"Example Provider");
        append(8, u"Custom Context");
        append(24, u"SP800_108_CTR_HMAC");
        bytes[44] = 3;
        const auto request = cng::unpack_resolution(bytes);
        ASSERT_TRUE(request);
        EXPECT_EQ(request->context, u"Custom Context");
        EXPECT_EQ(request->provider, u"Example Provider");
        EXPECT_EQ(request->function, u"SP800_108_CTR_HMAC");
        EXPECT_EQ(request->flags, 3u);
    }

    TEST(CngProtocolTest, PreservesNullAndEmptyStringsSeparately)
    {
        auto bytes = captured_aes_request;
        bytes[48] = 0;
        const auto request = cng::unpack_resolution(bytes);
        ASSERT_TRUE(request);
        EXPECT_FALSE(request->context);
        ASSERT_TRUE(request->function);
        EXPECT_TRUE(request->function->empty());
    }

    TEST(CngProtocolTest, SerializesAesPropertiesAndBothProviderImages)
    {
        const std::array providers{aes_provider()};
        const auto bytes = cng::pack_provider_refs(providers);
        EXPECT_EQ(field<uint32_t>(bytes, 0), 1u);
        const auto array = field<uint64_t>(bytes, 8);
        const auto provider = field<uint64_t>(bytes, array);
        EXPECT_EQ(field<uint32_t>(bytes, provider), 1u);
        EXPECT_EQ(field<uint32_t>(bytes, provider + 4), 0u);
        EXPECT_EQ(wire_string(bytes, field<uint64_t>(bytes, provider + 8)), u"AES");
        EXPECT_EQ(wire_string(bytes, field<uint64_t>(bytes, provider + 16)), u"Microsoft Primitive Provider");
        EXPECT_EQ(field<uint32_t>(bytes, provider + 24), 1u);
        EXPECT_EQ(field<uint32_t>(bytes, provider + 28), 0u);
        const auto properties = field<uint64_t>(bytes, provider + 32);
        const auto property = field<uint64_t>(bytes, properties);
        EXPECT_EQ(wire_string(bytes, field<uint64_t>(bytes, property)), u"KeyLength");
        EXPECT_EQ(field<uint32_t>(bytes, property + 8), 4u);
        EXPECT_EQ(field<uint32_t>(bytes, field<uint64_t>(bytes, property + 16)), 128u);
        for (const auto& [image_field, expected] :
             std::array<std::pair<size_t, std::u16string_view>, 2>{{{40, u"bcryptprimitives.dll"}, {48, u"cng.sys"}}})
        {
            const auto image = field<uint64_t>(bytes, provider + image_field);
            EXPECT_EQ(wire_string(bytes, field<uint64_t>(bytes, image)), expected);
            EXPECT_EQ(field<uint32_t>(bytes, image + 8), 1u);
            EXPECT_EQ(image % 8, 0u);
        }
    }

    TEST(CngProtocolTest, SerializesMultipleReferencesWithoutFixedNameSlots)
    {
        auto first = aes_provider();
        first.properties.clear();
        first.kernel_image.reset();
        auto second = first;
        second.interface_id = 7;
        second.function = u"SP800_108_CTR_HMAC";
        second.provider = u"A different provider with a longer name";
        second.properties = {{.name = u"Empty", .value = {}}, {.name = u"Binary", .value = {0, 0xff, 0, 0x12, 0x34}}};
        const std::array providers{first, second};
        const auto bytes = cng::pack_provider_refs(providers);
        EXPECT_EQ(field<uint32_t>(bytes, 0), 2u);
        const auto array = field<uint64_t>(bytes, 8);
        const auto one = field<uint64_t>(bytes, array);
        const auto two = field<uint64_t>(bytes, array + 8);
        EXPECT_NE(one, two);
        EXPECT_EQ(field<uint64_t>(bytes, one + 32), UINT64_MAX);
        EXPECT_EQ(field<uint64_t>(bytes, one + 48), UINT64_MAX);
        EXPECT_EQ(wire_string(bytes, field<uint64_t>(bytes, two + 8)), second.function);
        EXPECT_EQ(wire_string(bytes, field<uint64_t>(bytes, two + 16)), second.provider);
        const auto properties = field<uint64_t>(bytes, two + 32);
        const auto empty = field<uint64_t>(bytes, properties);
        EXPECT_EQ(field<uint32_t>(bytes, empty + 8), 0u);
        EXPECT_EQ(field<uint64_t>(bytes, empty + 16), UINT64_MAX);
        const auto binary = field<uint64_t>(bytes, properties + 8);
        const auto value = field<uint64_t>(bytes, binary + 16);
        EXPECT_EQ(field<uint32_t>(bytes, binary + 8), 5u);
        EXPECT_TRUE(std::equal(second.properties[1].value.begin(), second.properties[1].value.end(), bytes.begin() + value));
    }

    TEST(CngProtocolTest, EmptyArrayUsesNullWireSentinel)
    {
        const auto bytes = cng::pack_provider_refs({});
        EXPECT_EQ(bytes.size(), 16u);
        EXPECT_EQ(field<uint32_t>(bytes, 0), 0u);
        EXPECT_EQ(field<uint64_t>(bytes, 8), UINT64_MAX);
    }

    TEST(CngProtocolTest, HeaderOnlyRequestDecodesToDefaultArguments)
    {
        const auto request = cng::unpack_resolution(std::span{captured_aes_request}.first(8));
        ASSERT_TRUE(request);
        EXPECT_EQ(request->interface_id, 0u);
        EXPECT_EQ(request->mode, 0u);
        EXPECT_FALSE(request->function);
    }

    TEST(CngProtocolTest, RejectsAliasedStringsButAcceptsUnusedPadding)
    {
        auto bytes = captured_aes_request;
        uint64_t offset = 48;
        memcpy(bytes.data() + 8, &offset, 8);
        EXPECT_FALSE(cng::unpack_resolution(bytes));
        bytes = captured_aes_request;
        offset = 20;
        memcpy(bytes.data() + 24, &offset, 8);
        bytes[20] = 'A';
        const auto request = cng::unpack_resolution(bytes);
        ASSERT_TRUE(request);
        EXPECT_EQ(request->function, u"A");
        bytes = captured_aes_request;
        offset = 49;
        memcpy(bytes.data() + 24, &offset, 8);
        const auto unaligned = cng::unpack_resolution(bytes);
        ASSERT_TRUE(unaligned);
        EXPECT_EQ(unaligned->function, u"\u4500\u5300");
    }
}
