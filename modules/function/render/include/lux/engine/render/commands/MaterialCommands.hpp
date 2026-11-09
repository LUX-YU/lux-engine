#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/inputs/RasterMaterialInputs.hpp>

namespace lux::render::MaterialCommands
{
    struct Create final
    {
        static constexpr std::string_view name = "lux.render.material.create";
        static constexpr bool requires_frame_boundary = true;
        using Data = MaterialData;

        struct Request final
        {
            CapabilityImplementationId implementation;
            Data data;
        };

        [[nodiscard]] static CapabilityImplementationId implementationId(const Request& request) noexcept
        {
            return request.implementation;
        }

        // Published material identity. Referenced textures remain the Client's responsibility.
        struct Response final
        {
            RenderError error{};
            MaterialId material{};
        };
    };

    struct Update final
    {
        static constexpr std::string_view name = "lux.render.material.update";
        static constexpr bool requires_frame_boundary = true;
        using Data = MaterialUpdate;
        // Keep the program/table slot. A different Cooked program requires a new identity.
        struct Request final
        {
            Data data;
        };

        [[nodiscard]] static MaterialId resourceId(const Request& request) noexcept
        {
            return request.data.material;
        }

        using Response = AckResponse;
    };

    struct Release final
    {
        static constexpr std::string_view name = "lux.render.material.release";
        static constexpr bool requires_frame_boundary = true;
        static constexpr bool failure_safe = true;
        using Data = MaterialId;
        struct Request final
        {
            Data data;
        };

        [[nodiscard]] static MaterialId resourceId(const Request& request) noexcept
        {
            return request.data;
        }

        using Response = AckResponse;
    };
} // namespace lux::render::MaterialCommands
