#pragma once

#include <cstdint>

namespace lux::world
{
    struct PartitionDeclaration;
}

namespace lux::scene
{
    struct SceneSystemRegistration;

    struct GeometryDeclaration;

    struct ScenePluginExports final
    {
        std::uint32_t structure_size{sizeof(ScenePluginExports)};
        std::uint32_t interface_version{3};
        const SceneSystemRegistration* entries{};
        std::uint32_t count{};
        const GeometryDeclaration* geometries{};
        std::uint32_t geometry_count{};
        const world::PartitionDeclaration* partitions{};
        std::uint32_t partition_count{};
    };

    using GetScenePluginExports = const ScenePluginExports*() noexcept;
    inline constexpr const char* kScenePluginExportsSymbol = "lux_scene_exports_v3";
} // namespace lux::scene
