#pragma once

#include <lux/engine/render/effect/EffectCompileContext.hpp>

namespace lux::render::detail
{
    struct MeshVisibilityBuffers final
    {
        ResourceVersion transforms, alive, properties, views;
        ResourceVersion groups, candidates, chunks, visible, commands, dispatch;
    };

    struct MeshCullConstants final { std::uint32_t frustum_culling, shadow_pass; };

    struct MeshVisibilityOutputs final
    {
        ResourceVersion visible, commands;
    };

    [[nodiscard]] RenderResult<MeshVisibilityOutputs> addMeshVisibilityPasses(
        EffectCompileContext&, const std::string& pass_prefix, const MeshVisibilityBuffers&,
        MeshCullConstants
    ) noexcept;
} // namespace lux::render::detail
