#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/resources/instance/Data.hpp>

namespace lux::render::InstanceCommands
{
    struct CreateInstance final
    {
        static constexpr std::string_view name = "lux.render.mesh3d.create_instance";
        static constexpr bool requires_idle_resource = true;
        using Data = MeshInstanceData;

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
            MeshInstanceId instance{};
        };
    };

    struct CreateInstances final
    {
        static constexpr std::string_view name = "lux.render.mesh3d.create_instances";
        static constexpr bool requires_idle_resource = true;
        using Data = std::span<const MeshInstanceData>;

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
            std::vector<MeshInstanceId> instances;
        };
    };

    struct ReleaseInstance final
    {
        static constexpr std::string_view name = "lux.render.mesh3d.release_instance";
        static constexpr bool requires_idle_resource = true;
        static constexpr bool failure_safe = true;
        using Data = MeshInstanceId;
        struct Request final
        {
            Data data;
        };

        [[nodiscard]] static MeshInstanceId resourceId(const Request& request) noexcept
        {
            return request.data;
        }

        using Response = AckResponse;
    };
    struct Update final
    {
        static constexpr std::string_view name = "lux.render.instance.update";
        static constexpr bool requires_idle_resource = true;
        using Data = MeshInstanceUpdate;
        struct Request final
        {
            ResourceInstanceId resource;
            std::span<const TransformUpdate> transforms;
            std::span<const MeshInstanceUpdate> instances;
        };
        using Response = AckResponse;
        static ResourceInstanceId resourceId(const Request& value) noexcept { return value.resource; }
    };
} // namespace lux::render::InstanceCommands
