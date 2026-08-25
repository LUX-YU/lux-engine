#pragma once

#include <lux/engine/ecs/SystemAccessSpec.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>

#include <lux/engine/task/TaskGraph.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace lux::ecs
{
    class SystemContext;
}

namespace lux::ecs::detail
{
    struct SystemTaskTarget final
    {
        SystemSlot slot{};
        std::size_t scratch_index{};
        void (*update)(void*, SystemContext&) noexcept{};
        bool (*affinity_valid)(const void*) noexcept{};
        std::span<const SystemComponentAccess> allowed;
    };

    struct SystemPublishTarget final
    {
        std::size_t scratch_index{};
    };

    struct SystemScratchLaneLayout final
    {
        std::vector<std::uint64_t> write_storages;
    };

    struct SystemExecutionScratchLayout final
    {
        std::vector<SystemScratchLaneLayout> systems;
    };
}

namespace lux::ecs
{
    struct CompiledSystemTaskGraph::Impl final
    {
        lux::task::TaskGraph graph;
        std::vector<detail::SystemTaskTarget> system_tasks;
        std::vector<detail::SystemPublishTarget> publish_tasks;
        detail::SystemExecutionScratchLayout scratch_layout;
        SystemRegistryId registry_id{};
        SystemRelationsId relations_id{};
        std::uint64_t registry_revision{};
        std::uint64_t relations_revision{};
    };
}
