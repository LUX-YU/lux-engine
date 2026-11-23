#pragma once

#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/resource/view/Parameters.hpp>

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
} // namespace lux::render
