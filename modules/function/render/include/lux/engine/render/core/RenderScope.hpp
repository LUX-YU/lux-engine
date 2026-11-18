#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <variant>

namespace lux::render
{
    struct DeviceScope final
    {
        bool operator==(const DeviceScope&) const noexcept = default;
    };

    // Resolve the SceneId first, then look up the ViewId in that scene.
    struct ViewRequest final
    {
        SceneId scene;
        ViewId view;
        bool operator==(const ViewRequest&) const noexcept = default;
    };

    // Existing business ownership, not a Storage location or a new lifetime domain.
    using VRenderScope = std::variant<DeviceScope, SceneId, ViewRequest>;

} // namespace lux::render
