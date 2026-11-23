#pragma once

#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/resource/mesh/Data.hpp>

namespace lux::render::MeshCommands
{
    struct UploadMesh final
    {
        static constexpr std::string_view name = "lux.render.mesh3d.upload_mesh";
        using Data = rdesc::Mesh;

        struct Request final
        {
            ResourceInstanceId resource_id;
            // Keep the Mesh and all its arrays immutable until the terminal Response.
            // Abandon does not end this borrow; Host shutdown must settle all producers.
            const Data* mesh = nullptr;
        };

        [[nodiscard]] static ResourceInstanceId resourceId(const Request& request) noexcept
        {
            return request.resource_id;
        }

        struct Response final
        {
            RenderError error{};
            MeshId mesh{};
        };
    };

    struct ReleaseMesh final
    {
        static constexpr std::string_view name = "lux.render.mesh3d.release_mesh";
        static constexpr bool requires_idle_resource = true;
        static constexpr bool failure_safe = true;
        using Data = MeshId;
        struct Request final
        {
            Data data;
        };

        [[nodiscard]] static MeshId resourceId(const Request& request) noexcept
        {
            return request.data;
        }

        using Response = AckResponse;
    };

} // namespace lux::render::MeshCommands
