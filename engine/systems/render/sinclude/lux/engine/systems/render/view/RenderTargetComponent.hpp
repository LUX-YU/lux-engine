#pragma once

#include <lux/engine/ecs/ComponentAnnotations.hpp>

#include <lux/engine/render/core/Target.hpp>
#include <optional>

namespace lux::engine::render
{
    // Desired output, not a Window or an owning remote handle. The native surface outlives retirement.
    struct LUX_COMPONENT(
        schema = "lux.engine.render.RenderTargetComponent", version = 1,
        snapshot = REBUILD, semantic = RUNTIME_DERIVED
    ) RenderTargetComponent final
    {
        lux::render::TargetDesc description;
        std::optional<lux::render::SurfaceDescription> surface;
    };
}

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/systems/render/view/RenderTargetComponent.type_static_info.hpp>
#endif
