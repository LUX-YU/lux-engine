#pragma once

#include <lux/engine/description/MaterialProgramSet.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/features/shadow/ShadowProjection.hpp>

namespace lux::render { class EffectInitializationContext; }

namespace lux::render::detail
{
    struct MeshAttachmentInputs final
    {
        const std::string& key;
        std::span<const GraphResourceId> shadow_uploads;
    };

    struct MeshSurfaceOutput final
    {
        graph::PinSemanticId output;
        rdesc::EMaterialPurpose purpose;
        RenderResult<GraphPassId> (*append)(EffectCompileContext&, const MeshAttachmentInputs&, RasterWork) noexcept;
    };

    struct ShadowNodeConfiguration final
    {
        ShadowProjectionSettings projection{{1024, 1}, 100, 1, 0};
        bool frustum_culling = true;
    };

    RenderResult<void> initializeVisibility(EffectInitializationContext&, std::string_view) noexcept;
    RenderResult<void> appendCameraVisibility(EffectCompileContext&, const MeshSurfaceOutput*, bool) noexcept;
    RenderResult<void> appendShadowVisibility(EffectCompileContext&, const MeshSurfaceOutput&, bool) noexcept;
}
