#pragma once

#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/resource/material/Data.hpp>
#include <lux/engine/render/resource/material/MeshPipelines.hpp>
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
            // Indexed by EMaterialPurpose. Geometry and attachment state qualify the native pipeline separately.
            std::array<bool, 3> indexed_mesh_shader{};
        };

        [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
        {
            return request.resource_id;
        }
    };

    struct PrepareMeshPipelines final
    {
        static constexpr std::string_view name = "lux.render.material.mesh_pipelines.prepare";

        using Request = MaterialMeshPipelines;
        using Data = Request;
        using Response = AckResponse;

        [[nodiscard]] static MaterialProgramId resourceId(const Request& request) noexcept { return request.program; }
    };

    struct CreateInstance final
    {
        static constexpr std::string_view name = "lux.render.material.instance.create";
        static constexpr bool requires_idle_resource = true;

        using Request = MaterialInstanceData;
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
