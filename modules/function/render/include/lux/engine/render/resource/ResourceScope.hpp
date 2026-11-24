#pragma once

#include <lux/engine/render/runtime/SceneId.hpp>
#include <lux/engine/render/runtime/JobId.hpp>
#include <variant>

namespace lux::render
{
    struct DeviceScope final
    {
        bool operator==(const DeviceScope&) const noexcept = default;
    };

    // Resolve the SceneId first, then look up the ViewId in that scene.
    struct ViewScope final
    {
        SceneId scene;
        ViewId view;
        bool operator==(const ViewScope&) const noexcept = default;
    };

    // Existing business ownership, not a Storage location or a new lifetime domain.
    using VResourceScope = std::variant<DeviceScope, SceneId, ViewScope, RenderJobId>;

} // namespace lux::render
