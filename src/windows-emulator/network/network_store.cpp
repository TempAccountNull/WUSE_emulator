#include "network_store.hpp"

#include <utils/win.hpp>

namespace sogen::network
{
    uint32_t query_host_network_store(network_store_query& query)
    {
#ifdef _WIN32
        struct nsi_api
        {
            HMODULE module = LoadLibraryExW(L"nsi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            using get_parameter = uint32_t(WINAPI*)(uint32_t, const void*, uint32_t, const void*, uint32_t, uint32_t, void*, uint32_t,
                                                    uint32_t);
            using get_all = uint32_t(WINAPI*)(uint32_t, const void*, uint32_t, const void*, uint32_t, void*, uint32_t, void*, uint32_t,
                                              void*, uint32_t);
            using enumerate = uint32_t(WINAPI*)(uint32_t, uint32_t, const void*, uint32_t, void*, uint32_t, void*, uint32_t, void*,
                                                uint32_t, void*, uint32_t, uint32_t*);
            get_parameter parameter = module ? reinterpret_cast<get_parameter>(GetProcAddress(module, "NsiGetParameter")) : nullptr;
            get_all all = module ? reinterpret_cast<get_all>(GetProcAddress(module, "NsiGetAllParameters")) : nullptr;
            enumerate entries = module ? reinterpret_cast<enumerate>(GetProcAddress(module, "NsiEnumerateObjectsAllParameters")) : nullptr;

            ~nsi_api()
            {
                if (module)
                {
                    FreeLibrary(module);
                }
            }
        };

        static const nsi_api api;
        auto& key = query.buffers[0];
        auto& rw = query.buffers[1];
        auto& dynamic = query.buffers[2];
        auto& fixed = query.buffers[3];
        const auto data = [](network_store_buffer& buffer) -> void* { return buffer.bytes.empty() ? nullptr : buffer.bytes.data(); };
        switch (query.operation)
        {
        case network_store_operation::parameter:
            if (api.parameter)
            {
                return api.parameter(query.flags, query.module.data(), query.table, data(key), key.element_size, query.parameter_type,
                                     data(rw), rw.element_size, query.parameter_offset);
            }
            break;
        case network_store_operation::all_parameters:
            if (api.all)
            {
                return api.all(query.flags, query.module.data(), query.table, data(key), key.element_size, data(rw), rw.element_size,
                               data(dynamic), dynamic.element_size, data(fixed), fixed.element_size);
            }
            break;
        case network_store_operation::enumerate:
            if (api.entries)
            {
                return api.entries(query.flags, query.second_flags, query.module.data(), query.table, data(key), key.element_size, data(rw),
                                   rw.element_size, data(dynamic), dynamic.element_size, data(fixed), fixed.element_size, &query.count);
            }
            break;
        }
#else
        (void)query;
#endif
        return 50;
    }
}
