#pragma once

#include "vk_render_pass.hpp"

namespace sogen::gpu_bridge::synchronization_wire
{
    namespace codec = render_pass_wire;

    enum class operation : uint32_t
    {
        barrier = 1,
        barrier2,
        set_event,
        reset_event,
        wait_events,
        set_event2,
        reset_event2,
        wait_events2,
    };

    struct legacy_dependency
    {
        VkPipelineStageFlags source_stages{};
        VkPipelineStageFlags destination_stages{};
        VkDependencyFlags flags{};
        uint32_t memory_count{};
        const VkMemoryBarrier* memory{};
        uint32_t buffer_count{};
        const VkBufferMemoryBarrier* buffers{};
        uint32_t image_count{};
        const VkImageMemoryBarrier* images{};
    };

    struct command
    {
        operation op{};
        legacy_dependency legacy{};
        VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = 0,
            .pMemoryBarriers = nullptr,
            .bufferMemoryBarrierCount = 0,
            .pBufferMemoryBarriers = nullptr,
            .imageMemoryBarrierCount = 0,
            .pImageMemoryBarriers = nullptr,
        };
        VkEvent event{};
        VkPipelineStageFlags stage{};
        VkPipelineStageFlags2 stage2{};
        uint32_t event_count{};
        const VkEvent* events{};
        const VkDependencyInfo* dependencies{};
    };

    template <typename A>
    void visit(A& archive, legacy_dependency& dependency)
    {
        codec::value(archive, dependency.source_stages);
        codec::value(archive, dependency.destination_stages);
        codec::value(archive, dependency.flags);
        codec::value(archive, dependency.memory_count);
        codec::array(archive, dependency.memory, dependency.memory_count);
        codec::value(archive, dependency.buffer_count);
        codec::array(archive, dependency.buffers, dependency.buffer_count);
        codec::value(archive, dependency.image_count);
        codec::array(archive, dependency.images, dependency.image_count);
    }

    template <typename A>
    void visit(A& archive, command& value)
    {
        codec::value(archive, value.op);
        switch (value.op)
        {
        case operation::barrier:
            visit(archive, value.legacy);
            break;
        case operation::barrier2:
            codec::value(archive, value.dependency);
            break;
        case operation::set_event:
        case operation::reset_event:
            codec::handle(archive, value.event);
            codec::value(archive, value.stage);
            break;
        case operation::wait_events:
            codec::value(archive, value.event_count);
            codec::handles(archive, value.events, value.event_count);
            visit(archive, value.legacy);
            break;
        case operation::set_event2:
            codec::handle(archive, value.event);
            codec::value(archive, value.dependency);
            break;
        case operation::reset_event2:
            codec::handle(archive, value.event);
            codec::value(archive, value.stage2);
            break;
        case operation::wait_events2:
            codec::value(archive, value.event_count);
            codec::handles(archive, value.events, value.event_count);
            codec::array(archive, value.dependencies, value.event_count);
            break;
        default:
            throw codec::error("unknown synchronization operation");
        }
        if ((value.op == operation::wait_events || value.op == operation::wait_events2) && value.event_count == 0)
        {
            throw codec::error("event wait requires at least one event");
        }
    }

    inline std::vector<std::byte> encode(command value)
    {
        codec::writer writer;
        visit(writer, value);
        return std::move(writer.bytes);
    }

    inline void decode(codec::reader& reader, command& value)
    {
        visit(reader, value);
        reader.finish();
    }
}
