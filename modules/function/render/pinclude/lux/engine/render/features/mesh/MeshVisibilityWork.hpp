#pragma once

#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>

namespace lux::render::detail
{
    struct MeshVisibilityBuffers final
    {
        ResourceVersion transforms, alive, properties, views;
        ResourceVersion groups, candidates, chunks, visible, commands, dispatch;
    };

    struct MeshVisibilityOutputs final
    {
        ResourceVersion visible, commands;
    };

    [[nodiscard]] RenderResult<MeshVisibilityOutputs> addMeshVisibilityPasses(
        RenderGraphBuilder&, const std::string& pass_prefix, const MeshVisibilityBuffers&,
        bool frustum_culling, bool shadow_pass, std::optional<bool> indirect_first_instance = {}
    ) noexcept;
} // namespace lux::render::detail
