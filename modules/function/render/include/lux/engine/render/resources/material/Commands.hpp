#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/resources/material/Data.hpp>
#include <span>

namespace lux::render::MaterialCommands
{
    struct RegisterProgram final
    {
        static constexpr std::string_view name = "lux.render.material.program.register";
        static constexpr bool requires_idle_resource = true;
        using Data = rdesc::MaterialProgramSet;

        struct Request final
        {
            ResourceInstanceId resource_id;
            Data program;
        };

        struct Response final
        {
            RenderError error{};
            MaterialProgramId program{};
        };

        [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
        {
            return request.resource_id;
        }
    };

    struct CreateInstance final
    {
        static constexpr std::string_view name = "lux.render.material.instance.create";
        static constexpr bool requires_idle_resource = true;

        struct Request final
        {
            MaterialProgramId program;
            MaterialParameters parameters;
            // Referenced textures remain the client's responsibility.
            std::vector<TextureId> textures;
        };

        using Data = Request;

        struct Response final
        {
            RenderError error{};
            MaterialId material{};
        };

        [[nodiscard]] static MaterialProgramId resourceId(const Request& request) noexcept
        {
            return request.program;
        }
    };

    struct ReleaseInstance final
    {
        static constexpr std::string_view name = "lux.render.material.instance.release";
        static constexpr bool requires_idle_resource = true;
        static constexpr bool failure_safe = true;
        using Data = MaterialId;

        struct Request final
        {
            MaterialId material;
        };

        using Response = AckResponse;

        [[nodiscard]] static MaterialId resourceId(const Request& request) noexcept
        {
            return request.material;
        }
    };

    struct UnregisterProgram final
    {
        static constexpr std::string_view name = "lux.render.material.program.unregister";
        static constexpr bool requires_idle_resource = true;
        static constexpr bool failure_safe = true;
        using Data = MaterialProgramId;

        struct Request final
        {
            MaterialProgramId program;
        };

        using Response = AckResponse;

        [[nodiscard]] static MaterialProgramId resourceId(const Request& request) noexcept
        {
            return request.program;
        }
    };

    struct UpdateColor final
    {
        static constexpr std::string_view name = "lux.render.material.update_color";
        using Data = MaterialColorUpdate;

        struct Request final
        {
            ResourceInstanceId resource;
            std::span<const Data> updates;
        };

        using Response = AckResponse;

        [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
        {
            return request.resource;
        }
    };

    struct Update final
    {
        static constexpr std::string_view name = "lux.render.material.update";
        static constexpr bool requires_idle_resource = true;
        using Data = MaterialUpdate;
        // Keep the program/table slot. A different target program requires a new identity.
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

} // namespace lux::render::MaterialCommands
