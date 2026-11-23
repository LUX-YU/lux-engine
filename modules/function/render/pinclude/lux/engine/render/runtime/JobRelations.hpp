#pragma once
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/runtime/commands/JobCommands.hpp>
#include <lux/engine/render/device/DeviceId.hpp>
#include <lux/engine/render/runtime/JobId.hpp>
#include <span>
#include <vector>

namespace lux::render::detail
{
    struct JobRelations final
    {
        std::vector<RenderJobId> predecessors;
        std::vector<RenderJobId> successors;
    };

    // Cold, non-owning view over the existing Job table. Not a second Job registry.
    struct JobRelationEntry final
    {
        RenderJobId id;
        DeviceSessionId device;
        JobRelations* relations;
        std::vector<RenderJobId> products;
    };

    [[nodiscard]] RenderResult<void> validateJobDevices(std::span<const JobRelationEntry>) noexcept;

    [[nodiscard]] RenderResult<std::vector<RenderJobId>>
    orderRunningJobs(std::span<const JobRelationEntry> jobs, std::span<const RenderJobId> running) noexcept;
    // Atomically changes BOTH ends; rejected candidates leave adjacency and order intact.
    [[nodiscard]] RenderResult<void>
    setJobDependencies(
        std::span<const JobRelationEntry> jobs,
        const SetRenderJobDependencies::Request&,
        std::vector<RenderJobId>& running_order
    ) noexcept;
} // namespace lux::render::detail
