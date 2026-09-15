#include "../std_include.hpp"
#include "cng_resolver.hpp"

#include <utils/string.hpp>
#include <charconv>

namespace sogen::cng
{
    namespace
    {
        bool equal(const std::u16string_view left, const std::u16string_view right)
        {
            return utils::string::equals_ignore_case(left, right);
        }

        bool specified(const std::optional<std::u16string>& value)
        {
            return value && !value->empty();
        }

        struct invalid_catalog : std::runtime_error
        {
            invalid_catalog()
                : std::runtime_error("Invalid CNG registry catalog")
            {
            }
        };

        struct catalog_reader
        {
            registry_manager& registry;

            std::vector<std::u16string> children(const registry_key& key) const
            {
                std::vector<std::u16string> result;
                for (size_t i = 0; i < registry.get_sub_key_count(key); ++i)
                {
                    if (const auto name = registry.get_sub_key_name(key, i))
                    {
                        result.push_back(u8_to_u16(*name));
                    }
                }
                return result;
            }

            std::optional<registry_key> child(const registry_key& key, const std::u16string_view name) const
            {
                return registry.get_key(utils::path_key{std::filesystem::path{key.to_string()} / name});
            }

            uint32_t flags(const registry_key& key) const
            {
                const auto value = registry.get_value(key, "Flags");
                if (!value)
                {
                    return 0;
                }
                const auto result = value->as_dword();
                if (!result)
                {
                    throw invalid_catalog{};
                }
                return *result;
            }

            std::vector<std::u16string> strings(const registry_key& key, const std::string_view name) const
            {
                const auto value = registry.get_value(key, name);
                if (!value)
                {
                    return {};
                }
                if (value->type != REG_MULTI_SZ || value->data.size() % 2)
                {
                    throw invalid_catalog{};
                }
                std::vector<std::u16string> result;
                std::u16string current;
                for (size_t i = 0; i < value->data.size(); i += 2)
                {
                    char16_t character{};
                    memcpy(&character, value->data.data() + i, 2);
                    if (character)
                    {
                        current.push_back(character);
                    }
                    else if (current.empty())
                    {
                        return result;
                    }
                    else
                    {
                        result.push_back(std::move(current));
                        current.clear();
                    }
                }
                if (!current.empty())
                {
                    throw invalid_catalog{};
                }
                return result;
            }
        };

        uint32_t interface_number(const std::u16string& name)
        {
            const auto narrow = u16_to_u8(name);
            uint32_t value{};
            const auto [end, error] = std::from_chars(narrow.data(), narrow.data() + narrow.size(), value, 16);
            return error == std::errc{} && end == narrow.data() + narrow.size() ? value : 0;
        }

        struct function_configuration
        {
            std::u16string name;
            uint32_t flags{};
            std::vector<std::u16string> providers;
            std::vector<property_reference> properties;
        };

        struct interface_configuration
        {
            uint32_t id{};
            std::vector<std::u16string> functions;
            std::vector<function_configuration> definitions;
        };

        struct context_configuration
        {
            std::u16string name;
            bool domain{};
            uint32_t flags{};
            std::vector<interface_configuration> interfaces;
        };

        struct image_interface
        {
            uint32_t id{};
            uint32_t flags{};
            std::vector<std::u16string> functions;
        };

        struct provider_image
        {
            uint32_t mode{};
            std::optional<std::u16string> name;
            std::vector<image_interface> interfaces;
        };

        struct registered_provider
        {
            std::u16string name;
            std::vector<provider_image> images;
        };

        bool contains(const std::vector<std::u16string>& values, const std::u16string_view wanted)
        {
            return std::ranges::any_of(values, [&](const auto& value) { return equal(value, wanted); });
        }

        template <typename T>
        const T* find_interface(const std::vector<T>& values, const uint32_t id)
        {
            const auto found = std::ranges::find(values, id, &T::id);
            return found == values.end() ? nullptr : &*found;
        }

        const function_configuration* find_function(const interface_configuration& interface, const std::u16string_view name)
        {
            const auto found = std::ranges::find_if(interface.definitions, [&](const auto& value) { return equal(value.name, name); });
            return found == interface.definitions.end() ? nullptr : &*found;
        }

        class resolver
        {
          public:
            explicit resolver(registry_manager& registry)
                : reader_{registry}
            {
                const auto root =
                    registry.get_key(utils::path_key{u"\\registry\\machine\\system\\CurrentControlSet\\Control\\Cryptography"});
                if (!root)
                {
                    return;
                }
                if (const auto configuration = reader_.child(*root, u"Configuration"))
                {
                    load_contexts(*configuration);
                }
                if (const auto providers = reader_.child(*root, u"Providers"))
                {
                    load_providers(*providers);
                }
            }

            resolution_result run(const resolution_request& request) const
            {
                auto id = request.interface_id;
                if ((!id && !specified(request.function)) || (id && !(id >= 1 && id <= 7) && !(id >= 0x10001 && id <= 0x10004)) ||
                    request.mode < 1 || request.mode > 3 || (request.flags & ~3u))
                {
                    return {.status = STATUS_INVALID_PARAMETER, .providers = {}};
                }
                if (specified(request.function))
                {
                    const auto determined = determine_interface(*request.function);
                    if (!determined)
                    {
                        return {.status = STATUS_NOT_FOUND, .providers = {}};
                    }
                    if (id && id != determined)
                    {
                        return {.status = STATUS_NOT_FOUND, .providers = {}};
                    }
                    id = determined;
                }
                const auto contexts = select_contexts(request.context);
                if (!std::ranges::any_of(contexts, [&](const auto* context) { return find_interface(context->interfaces, id); }))
                {
                    return {.status = STATUS_NOT_FOUND, .providers = {}};
                }
                std::vector<provider_reference> result;
                if (specified(request.provider))
                {
                    for (const auto& provider : providers_)
                    {
                        if (equal(provider.name, *request.provider))
                        {
                            add_provider(provider, id, specified(request.function) ? request.function : std::nullopt, request.mode,
                                         contexts, result);
                            break;
                        }
                    }
                }
                else if (specified(request.function))
                {
                    resolve_function(id, *request.function, request, contexts, result);
                }
                else
                {
                    std::vector<std::u16string> seen;
                    const auto resolve_once = [&](const std::u16string& function) {
                        if (contains(seen, function))
                        {
                            return false;
                        }
                        seen.push_back(function);
                        const auto count = result.size();
                        resolve_function(id, function, request, contexts, result);
                        return result.size() != count && !(request.flags & 1);
                    };
                    bool stop = false;
                    for (const auto* context : contexts)
                    {
                        if (const auto* interface = find_interface(context->interfaces, id))
                        {
                            for (const auto& function : interface->functions)
                            {
                                if (resolve_once(function))
                                {
                                    stop = true;
                                    break;
                                }
                            }
                        }
                        if (stop || (context->flags & 1))
                        {
                            stop = true;
                            break;
                        }
                    }
                    if (!stop)
                    {
                        for (const auto& provider : providers_)
                        {
                            for (const auto& image : provider.images)
                            {
                                if (const auto* interface = find_interface(image.interfaces, id))
                                {
                                    for (const auto& function : interface->functions)
                                    {
                                        if (resolve_once(function))
                                        {
                                            stop = true;
                                            break;
                                        }
                                    }
                                }
                                if (stop)
                                {
                                    break;
                                }
                            }
                            if (stop)
                            {
                                break;
                            }
                        }
                    }
                }
                return {.status = result.empty() ? STATUS_NOT_FOUND : STATUS_SUCCESS, .providers = std::move(result)};
            }

          private:
            catalog_reader reader_;
            std::vector<context_configuration> contexts_;
            std::vector<registered_provider> providers_;

            void load_contexts(const registry_key& configuration)
            {
                for (const auto domain : {false, true})
                {
                    const auto table = reader_.child(configuration, domain ? u"Domain" : u"Local");
                    if (!table)
                    {
                        continue;
                    }
                    for (const auto& name : reader_.children(*table))
                    {
                        const auto key = reader_.child(*table, name);
                        if (!key)
                        {
                            continue;
                        }
                        context_configuration context{.name = name, .domain = domain, .flags = reader_.flags(*key)};
                        for (const auto& interface_name : reader_.children(*key))
                        {
                            const auto id = interface_number(interface_name);
                            if (!id)
                            {
                                continue;
                            }
                            const auto interface_key = reader_.child(*key, interface_name);
                            if (!interface_key)
                            {
                                continue;
                            }
                            interface_configuration interface{.id = id, .functions = reader_.strings(*interface_key, "Functions")};
                            for (const auto& function : reader_.children(*interface_key))
                            {
                                const auto function_key = reader_.child(*interface_key, function);
                                if (!function_key)
                                {
                                    continue;
                                }
                                function_configuration definition{.name = function,
                                                                  .flags = reader_.flags(*function_key),
                                                                  .providers = reader_.strings(*function_key, "Providers")};
                                if (const auto properties = reader_.child(*function_key, u"Properties"))
                                {
                                    for (size_t i = 0; i < reader_.registry.get_value_count(*properties); ++i)
                                    {
                                        const auto value = reader_.registry.get_value(*properties, i);
                                        if (!value)
                                        {
                                            continue;
                                        }
                                        property_reference property{.name = u8_to_u16(value->name)};
                                        property.value.resize(value->data.size());
                                        memcpy(property.value.data(), value->data.data(), value->data.size());
                                        definition.properties.push_back(std::move(property));
                                    }
                                }
                                interface.definitions.push_back(std::move(definition));
                            }
                            context.interfaces.push_back(std::move(interface));
                        }
                        contexts_.push_back(std::move(context));
                    }
                }
            }

            void load_providers(const registry_key& root)
            {
                for (const auto& name : reader_.children(root))
                {
                    const auto key = reader_.child(root, name);
                    if (!key)
                    {
                        continue;
                    }
                    registered_provider provider{.name = name};
                    for (const auto mode : {1u, 2u})
                    {
                        const auto image_key = reader_.child(*key, mode == 1 ? u"UM" : u"KM");
                        if (!image_key)
                        {
                            continue;
                        }
                        provider_image image{.mode = mode};
                        if (const auto value = reader_.registry.get_value(*image_key, "Image"))
                        {
                            image.name = value->as_string();
                            if (!image.name)
                            {
                                throw invalid_catalog{};
                            }
                        }
                        for (const auto& interface_name : reader_.children(*image_key))
                        {
                            const auto id = interface_number(interface_name);
                            if (!id)
                            {
                                continue;
                            }
                            const auto interface_key = reader_.child(*image_key, interface_name);
                            if (!interface_key)
                            {
                                continue;
                            }
                            image.interfaces.push_back({.id = id,
                                                        .flags = reader_.flags(*interface_key),
                                                        .functions = reader_.strings(*interface_key, "Functions")});
                        }
                        provider.images.push_back(std::move(image));
                    }
                    providers_.push_back(std::move(provider));
                }
            }

            uint32_t determine_interface(const std::u16string& function) const
            {
                if (equal(function, u"KEY_STORAGE"))
                {
                    return 0x10001;
                }
                uint32_t result = 0;
                const auto record = [&](const uint32_t id) {
                    if (id == 0x10001)
                    {
                        return;
                    }
                    if (result && result != id)
                    {
                        throw invalid_catalog{};
                    }
                    result = id;
                };
                for (const auto& context : contexts_)
                {
                    for (const auto& interface : context.interfaces)
                    {
                        if (contains(interface.functions, function) || find_function(interface, function))
                        {
                            record(interface.id);
                        }
                    }
                }
                for (const auto& provider : providers_)
                {
                    for (const auto& image : provider.images)
                    {
                        for (const auto& interface : image.interfaces)
                        {
                            if (contains(interface.functions, function))
                            {
                                record(interface.id);
                            }
                        }
                    }
                }
                return result;
            }

            std::vector<const context_configuration*> select_contexts(const std::optional<std::u16string>& requested) const
            {
                const auto find = [&](const bool domain, const std::u16string_view name) -> const context_configuration* {
                    for (const auto& context : contexts_)
                    {
                        if (context.domain == domain && equal(context.name, name))
                        {
                            return &context;
                        }
                    }
                    return nullptr;
                };
                const auto* local = specified(requested) ? find(false, *requested) : nullptr;
                const auto* domain = specified(requested) ? find(true, *requested) : nullptr;
                if (specified(requested) && !local && !domain)
                {
                    return {};
                }
                const bool isolated = (local && (local->flags & 1)) || (domain && (domain->flags & 1));
                std::vector<const context_configuration*> result;
                for (const auto* context :
                     {local, isolated ? nullptr : find(false, u"Default"), domain, isolated ? nullptr : find(true, u"Default")})
                {
                    if (context && std::ranges::find(result, context) == result.end())
                    {
                        result.push_back(context);
                    }
                }
                std::ranges::stable_sort(result, [](const auto* left, const auto* right) {
                    return (left->domain && (left->flags & 0x10000)) > (right->domain && (right->flags & 0x10000));
                });
                return result;
            }

            static std::vector<property_reference> properties(const uint32_t id, const std::u16string& function,
                                                              const std::vector<const context_configuration*>& contexts)
            {
                std::vector<property_reference> result;
                for (const auto* context : contexts)
                {
                    const auto* interface = find_interface(context->interfaces, id);
                    const auto* definition = interface ? find_function(*interface, function) : nullptr;
                    if (!definition)
                    {
                        continue;
                    }
                    for (const auto& property : definition->properties)
                    {
                        if (!std::ranges::any_of(result, [&](const auto& previous) { return equal(previous.name, property.name); }))
                        {
                            result.push_back(property);
                        }
                    }
                    if (definition->flags & 1)
                    {
                        break;
                    }
                }
                return result;
            }

            static bool add_provider(const registered_provider& provider, const uint32_t id, const std::optional<std::u16string>& function,
                                     const uint32_t mode, const std::vector<const context_configuration*>& contexts,
                                     std::vector<provider_reference>& result)
            {
                provider_reference reference{.interface_id = id, .function = function, .provider = provider.name};
                for (const auto needed : {1u, 2u})
                {
                    if (!(mode & needed))
                    {
                        continue;
                    }
                    const auto image = std::ranges::find(provider.images, needed, &provider_image::mode);
                    if (image == provider.images.end())
                    {
                        return false;
                    }
                    const auto* interface = find_interface(image->interfaces, id);
                    if (!interface || (specified(function) && !contains(interface->functions, *function)))
                    {
                        return false;
                    }
                    auto& destination = needed == 1 ? reference.user_image : reference.kernel_image;
                    destination = image_reference{.name = image->name, .flags = interface->flags};
                }
                if (function)
                {
                    reference.properties = properties(id, *function, contexts);
                }
                result.push_back(std::move(reference));
                return true;
            }

            void resolve_function(const uint32_t id, const std::u16string& function, const resolution_request& request,
                                  const std::vector<const context_configuration*>& contexts, std::vector<provider_reference>& result) const
            {
                std::vector<std::u16string> seen;
                const auto try_provider = [&](const std::u16string& name, const std::u16string& function_name) {
                    if (contains(seen, name))
                    {
                        return false;
                    }
                    seen.push_back(name);
                    for (const auto& provider : providers_)
                    {
                        if (equal(provider.name, name))
                        {
                            return add_provider(provider, id, function_name, request.mode, contexts, result);
                        }
                    }
                    return false;
                };
                for (const auto* context : contexts)
                {
                    const auto* interface = find_interface(context->interfaces, id);
                    const auto* definition = interface ? find_function(*interface, function) : nullptr;
                    if (!definition)
                    {
                        continue;
                    }
                    for (const auto& provider : definition->providers)
                    {
                        if (try_provider(provider, definition->name) && !(request.flags & 2))
                        {
                            return;
                        }
                    }
                    if (definition->flags & 1)
                    {
                        return;
                    }
                }
                for (const auto& provider : providers_)
                {
                    if (try_provider(provider.name, function) && !(request.flags & 2))
                    {
                        return;
                    }
                }
            }
        };
    }

    resolution_result resolve(registry_manager& registry, const resolution_request& request)
    {
        try
        {
            return resolver{registry}.run(request);
        }
        catch (const invalid_catalog&)
        {
            return {.status = STATUS_DATA_ERROR, .providers = {}};
        }
    }
}
