#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/core/RenderScope.hpp>
#include <lux/engine/render/core/Resource.hpp>

namespace lux::render
{
    struct ResourceRequest final
    {
        VRenderScope scope;
        ResourceNameView declaration;
    };

    struct ResourceResponse final
    {
        RenderError error{};
        ResourceTypeId type;
        ResourceInstanceId resource_id;
    };

    struct QueryResource final
    {
        static constexpr std::string_view name = "lux.render.resource.query";
        static constexpr bool requires_frame_boundary = false;
        using Request = ResourceRequest;
        using Response = ResourceResponse;
    };

    struct GetOrCreateResource final
    {
        static constexpr std::string_view name = "lux.render.resource.get_or_create";
        static constexpr bool requires_frame_boundary = true;
        using Request = ResourceRequest;
        using Response = ResourceResponse;
    };
} // namespace lux::render
