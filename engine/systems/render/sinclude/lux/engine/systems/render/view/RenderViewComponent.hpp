#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>

#include <lux/engine/render/core/RenderScope.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <optional>

namespace lux::engine::render
{
    // Adopted remote identities. Reaction transfers and clears these values; this component has no behavior.
    struct LUX_COMPONENT(
        schema = "lux.engine.render.RenderViewComponent", version = 1,
        snapshot = REBUILD, semantic = RUNTIME_DERIVED
    ) RenderViewComponent final
    {
        lux::render::ViewId view;
        lux::render::TargetId target;
        lux::render::AttachmentId attachment;
        lux::render::TargetDesc description;
        std::uint64_t epoch = 0;
        std::optional<lux::render::SurfaceDescription> surface;

    };
}

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/systems/render/view/RenderViewComponent.type_static_info.hpp>
#endif
