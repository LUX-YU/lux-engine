#pragma once

#include <lux/engine/render/device/DeviceId.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>
#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/resource/ResourceScope.hpp>
#include <string>
#include <vector>
#include <lux/engine/render/resource/ResourceDefinition.hpp>

namespace lux::render
{
    struct ResourceRequest final
    {
        DeviceSessionId device;
        VResourceScope scope;
        ResourceNameView declaration;
        std::string binding{}; // Empty selects the default instance of this type in the scope.
    };

    struct ResourceResponse final
    {
        RenderError error{};
        ResourceTypeId type;
        ResourceInstanceId resource_id;
        ServerInstanceId server{};
        [[nodiscard]] ResourceAddress address() const noexcept { return {server, resource_id}; }
    };

    struct QueryResource final
    {
        static constexpr std::string_view name = "lux.render.resource.query";
        using Request = ResourceRequest;
        using Response = ResourceResponse;
    };

    struct RemoveResource final
    {
        static constexpr std::string_view name = "lux.render.resource.remove";
        static constexpr bool failure_safe = true;
        using Request = ResourceAddress;
        using Response = AckResponse;
    };

    struct ListResourceJobs final
    {
        static constexpr std::string_view name = "lux.render.resource.jobs";
        static constexpr bool failure_safe = true;
        using Request = ResourceAddress;

        struct Response final
        {
            RenderError error{};
            std::vector<RenderJobId> jobs;
        };
    };

    struct GetOrCreateResource final
    {
        static constexpr std::string_view name = "lux.render.resource.get_or_create";
        using Request = ResourceRequest;
        using Response = ResourceResponse;
    };
} // namespace lux::render
