#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <variant>

namespace lux::render
{
    struct GlobalRenderScope final
    {
        bool operator==(const GlobalRenderScope&) const noexcept = default;
    };

    struct ViewRequest final
    {
        SceneId scene;
        ViewId view;
        bool operator==(const ViewRequest&) const noexcept = default;
    };

    // Existing business ownership, not a Storage location or a new lifetime domain.
    using VRenderScope = std::variant<GlobalRenderScope, SceneId, ViewRequest, AttachmentId>;

} // namespace lux::render
