"""Generate pointer-free feature layouts from Sogen's pinned Vulkan registry."""
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
registry = ET.parse(ROOT/'deps/Vulkan-Headers/registry/vk.xml').getroot()
types = {t.get('name') or t.findtext('name'): t for t in registry.findall('types/type') if t.get('name') or t.findtext('name')}
available = set()
for provider in [*registry.findall('feature'), *registry.findall('extensions/extension')]:
    if not {'vulkan', 'vulkanbase'}.intersection(provider.get('api', provider.get('supported', '')).split(',')):
        continue
    if provider.get('platform') not in (None, 'win32') or provider.get('provisional') == 'true':
        continue
    for requirement in provider.findall('require'):
        if requirement.get('api') and not {'vulkan', 'vulkanbase'}.intersection(requirement.get('api').split(',')):
            continue
        available.update(t.get('name') for t in requirement.findall('type'))
for name in list(available):
    seen=set()
    while name in types and types[name].get('alias') and name not in seen:
        seen.add(name)
        name = types[name].get('alias')
        available.add(name)

rows=[]
for name, t in sorted(types.items()):
    if name not in available or t.get('alias') or 'VkPhysicalDeviceFeatures2' not in t.get('structextends', '').split(','):
        continue
    members = [m for m in t.findall('member') if m.get('api') in (None, 'vulkan', 'vulkanbase')]
    assert [m.findtext('name') for m in members[:2]] == ['sType','pNext'], name
    fields = members[2:]
    assert fields and all(m.findtext('type') == 'VkBool32' and '*' not in ''.join(m.itertext()) and '[' not in ''.join(m.itertext()) for m in fields), name
    rows.append((name, members[0].get('values'), fields[0].findtext('name'), fields[-1].findtext('name'), str(len(fields))))
base = types['VkPhysicalDeviceFeatures'].findall('member')
assert all(m.findtext('type') == 'VkBool32' for m in base)
rows.insert(0, ('VkPhysicalDeviceFeatures2', 'VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2', 'features', 'features', str(len(base))))

out = '''#pragma once

// Generated from Vulkan-Headers 31386378257ac8653ce5b32c93baec385259ebbe (1.4.361).
// The wire contains only named VkBool32 fields: native tail padding is never a feature.
#include <cstddef>
#include <vulkan/vulkan_core.h>

namespace sogen::gpu_bridge
{
    struct feature_layout
    {
        size_t structure_size;
        size_t body_size;
    };

    // This generated registry dispatch has one case per supported structure.
    // NOLINTNEXTLINE(hicpp-function-size, readability-function-size)
    inline feature_layout registry_feature_layout(VkStructureType type)
    {
        switch (type)
        {
'''
for name, stype, first, last, count in rows:
    out += f'''        case {stype}:
            static_assert(offsetof({name}, {first}) == 2 * sizeof(void*));
            static_assert(offsetof({name}, {last}) + sizeof({name}::{last}) -
                          offsetof({name}, {first}) == {count} * sizeof(VkBool32));
            return {{.structure_size = sizeof({name}), .body_size = {count} * sizeof(VkBool32)}};
'''
out += '''        default:
            return {};
        }
    }
}
'''
(ROOT/'src/gpu-bridge-protocol/vk_feature_layouts.hpp').write_text(out, newline='\n')
