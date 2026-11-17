#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/resources/lighting/Data.hpp>

namespace lux::render::LightingCommands
{
    struct CreateLights final
    {
        static constexpr std::string_view name = "lux.render.lighting3d.create_lights";
        static constexpr bool requires_frame_boundary = true;
        using Data = LightData;

        struct Request final
        {
            ResourceInstanceId resource_id;
            Data data;
        };

        [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
        {
            return request.resource_id;
        }

        struct Response final
        {
            RenderError error{};
            std::uint64_t sequence = 0;
            std::vector<LightId> lights;
        };
    };

    struct ReleaseLight final
    {
        static constexpr std::string_view name = "lux.render.lighting3d.release_light";
        static constexpr bool requires_frame_boundary = true;
        static constexpr bool failure_safe = true;
        using Data = LightId;

        struct Request final
        {
            LightId light;
            std::uint64_t sequence = 0;
        };

        [[nodiscard]] static LightId resourceId(const Request& request) noexcept
        {
            return request.light;
        }

        struct Response final
        {
            RenderError error{};
            std::uint64_t sequence = 0;
        };
    };
    struct Update final
    {
        static constexpr std::string_view name = "lux.render.lighting.update";
        static constexpr bool requires_frame_boundary = true;
        using Data = DirectLightUpdate;
        struct Request final
        {
            ResourceInstanceId resource;
            std::span<const DirectLightUpdate> values;
        };
        using Response = AckResponse;
        static ResourceInstanceId resourceId(const Request& value) noexcept { return value.resource; }
    };
} // namespace lux::render::LightingCommands
