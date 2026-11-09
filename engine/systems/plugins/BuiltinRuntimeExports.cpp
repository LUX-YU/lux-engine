#include <algorithm>
#include <array>
#include <lux/engine/scene/FixedPartitionStreamingSystem.hpp>
#include <lux/engine/GeometryDeclaration.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/ScenePluginExports.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/ecs/ComponentPluginExports.hpp>
#include <lux/engine/ecs/HierarchySchema.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/ecs/VisualSchema.hpp>
#include <lux/engine/world/SinglePartitionAlgorithm.hpp>
#include <vector>

#if defined(_WIN32)
#define LUX_BUILTIN_EXPORT __declspec(dllexport)
#else
#define LUX_BUILTIN_EXPORT __attribute__((visibility("default")))
#endif

namespace
{
    struct MeshGeometry final
    {
        using PartitionInput = lux::world::ObjectPartitionInput;
        static constexpr std::string_view name = "lux.geometry.mesh3d";
        static constexpr std::uint32_t version = 1;

        static std::span<const lux::ecs::ComponentSchema> components() noexcept
        {
            using namespace lux::ecs;
            static const std::array schemas{
                *std::ranges::find(
                    visualComponentSchemas(), lux::cxx::make_basic_type_info<Mesh3D>(), &ComponentSchema::cpp_type
                )
            };
            return schemas;
        }

        static std::span<const lux::system::SystemDataContract> data() noexcept
        {
            return {};
        }
    };
} // namespace

// This composition is consumed by both the DLL and its build-time description writer.
// No Editor or application maintains a corresponding list.
extern "C" LUX_BUILTIN_EXPORT const lux::scene::ScenePluginExports* lux_scene_exports_v3() noexcept
{
    static const std::array values{
        lux::scene::transformSystemRegistration(),
        lux::scene::builtinMeshQuerySystemRegistration(),
        lux::scene::worldLoadingSystemRegistration(),
        lux::scene::fixedPartitionStreamingSystemRegistration()
    };
    static const std::array geometries{lux::scene::geometryDeclaration<MeshGeometry>()};
    static const std::array partitions{lux::world::partitionDeclaration<lux::world::SinglePartitionAlgorithm>()};
    static const lux::scene::ScenePluginExports table{
        .entries = values.data(),
        .count = static_cast<std::uint32_t>(values.size()),
        .geometries = geometries.data(),
        .geometry_count = static_cast<std::uint32_t>(geometries.size()),
        .partitions = partitions.data(),
        .partition_count = static_cast<std::uint32_t>(partitions.size())
    };
    return &table;
}

extern "C" LUX_BUILTIN_EXPORT const lux::ecs::ComponentPluginExports* lux_component_exports_v2() noexcept
{
    static const auto values = []
    {
        using namespace lux::ecs;
        std::vector<ComponentSchema> result;
        const auto append = [&](auto range) { result.insert(result.end(), range.begin(), range.end()); };
        append(transformComponentSchemas());
        append(hierarchyComponentSchemas());
        append(visualComponentSchemas());
        append(lux::scene::worldLoadingComponentSchemas());
        return result;
    }();
    static const lux::ecs::ComponentPluginExports
        table{sizeof(table), 2, values.data(), static_cast<std::uint32_t>(values.size())};
    return &table;
}
