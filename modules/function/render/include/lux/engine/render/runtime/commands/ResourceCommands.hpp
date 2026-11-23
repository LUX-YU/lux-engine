#pragma once

#include <lux/engine/render/device/DeviceId.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>
#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/resource/ResourceScope.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>

namespace lux::render
{
    struct ResourceRequest final
    {
        DeviceSessionId device;
        VResourceScope scope;
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
        using Request = ResourceRequest;
        using Response = ResourceResponse;
    };

    struct GetOrCreateResource final
    {
        static constexpr std::string_view name = "lux.render.resource.get_or_create";
        using Request = ResourceRequest;
        using Response = ResourceResponse;
    };
} // namespace lux::render
