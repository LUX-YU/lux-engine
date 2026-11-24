#pragma once

#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/resource/transform/Data.hpp>

namespace lux::render
{
    namespace TransformCommands
    {
        struct Create final
        {
            static constexpr std::string_view name = "lux.render.transforms.create";
            using Data = TransformMatrix;

            struct Request final
            {
                ResourceInstanceId resource;
                std::span<const TransformMatrix> matrices;
            };

            struct Response final
            {
                RenderError error{};
                std::vector<TransformRef> transforms;
            };

            [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
            {
                return request.resource;
            }
        };

        struct Update final
        {
            static constexpr std::string_view name = "lux.render.transforms.update";
            using Data = TransformRecordUpdate;

            struct Request final
            {
                ResourceInstanceId resource;
                std::span<const TransformRecordUpdate> values;
            };

            using Response = AckResponse;

            [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
            {
                return request.resource;
            }
        };

        struct Release final
        {
            static constexpr std::string_view name = "lux.render.transforms.release";
            using Data = TransformRef;
            using Request = TransformRef;
            using Response = AckResponse;

            [[nodiscard]] static TransformRef resourceId(const Request& request) noexcept { return request; }
        };
    } // namespace TransformCommands
} // namespace lux::render
