#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/resources/material/Data.hpp>

namespace lux::render::MaterialCommands
{
    struct RegisterProgram final
    {
        static constexpr std::string_view name = "lux.render.material.program.register";
        static constexpr bool requires_idle_resource = true;

        struct Request final
        {
            ResourceInstanceId resource_id;
            rdesc::MaterialProgramSet program;
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

    struct Create final
    {
        static constexpr std::string_view name = "lux.render.material.create";
        static constexpr bool requires_idle_resource = true;
        using Data = MaterialData;

        struct Request final
        {
            ResourceInstanceId resource_id;
            Data data;
        };

        [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
        {
            return request.resource_id;
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

    struct Release final
    {
        static constexpr std::string_view name = "lux.render.material.release";
        static constexpr bool requires_idle_resource = true;
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
