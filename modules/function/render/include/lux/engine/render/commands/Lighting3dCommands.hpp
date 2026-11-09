#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/inputs/DirectLightInputs.hpp>

namespace lux::render::Lighting3dCommands
{
    struct CreateLights final
    {
        static constexpr std::string_view name = "lux.render.lighting3d.create_lights";
        static constexpr bool requires_frame_boundary = true;
        using Data = LightData;

        struct Request final
        {
            CapabilityImplementationId implementation;
            SceneId scene;
            Data data;
        };

        [[nodiscard]] static CapabilityImplementationId implementationId(const Request& request) noexcept
        {
            return request.implementation;
        }

        [[nodiscard]] static SceneId sceneId(const Request& request) noexcept
        {
            return request.scene;
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
} // namespace lux::render::Lighting3dCommands
