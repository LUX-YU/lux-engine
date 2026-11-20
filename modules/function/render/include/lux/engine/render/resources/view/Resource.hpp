#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resources/BuiltinResources.hpp>
#include <lux/engine/render/resources/view/Parameters.hpp>

namespace lux::render
{
    namespace ViewParameterCommands
    {
        struct Update final
        {
            static constexpr std::string_view name = "lux.render.view_parameters.update";
            using Data = ViewGpuParameters;

            struct Request final
            {
                ResourceInstanceId resource;
                std::span<const ViewGpuParameters> values;
            };

            using Response = AckResponse;

            [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
            {
                return request.resource;
            }
        };
    }

    struct ViewParameterResource final
    {
        using Commands = std::tuple<ViewParameterCommands::Update>;
        using PerFrameData = std::tuple<ViewGpuParameters>;
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{
            kViewParameterResourceName,
            0,
            0,
            {},
            "Complete view parameters supplied by the application.",
            EResourceDomain::VIEW,
            resourceCommandDescriptors(static_cast<Commands*>(nullptr)),
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };
} // namespace lux::render
