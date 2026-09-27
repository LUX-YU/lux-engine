#pragma once

#include <lux/engine/math/MeshBVH.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>

#include <lux/engine/scene/mesh_query/visibility.h>
#include <lux/engine/simulation/ecs/Registry.hpp>

#include <array>
#include <memory>
#include <span>

namespace lux::scene
{
    enum class EMeshQueryError
    {
        INVALID_INPUT,
        INVALID_GEOMETRY,
        INVALID_TRANSFORM,
        NOT_READY,
        ASSET_FAILURE,
        CANCELLED,
        CAPACITY
    };

    struct MeshQueryFailure
    {
        EMeshQueryError code{};
        simulation::ecs::Entity entity{simulation::ecs::NullEntity};
        asset::AssetId mesh{};
    };

    template <class T> using QueryResult = lux::cxx::expected<T, MeshQueryFailure>;

    // Immutable local geometry. Prepare once on Process, then share the same
    // value across instances and Scenes. No Renderer resource is borrowed.
    struct MeshQueryGeometry
    {
        math::MeshBVH triangles;
        math::AABB bounds;

        [[nodiscard]] static LUX_ENGINE_SCENE_MESH_QUERY_PUBLIC QueryResult<std::shared_ptr<const MeshQueryGeometry>>
        build(std::span<const Eigen::Vector3f> positions, std::span<const std::uint32_t> indices);
    };

    struct RayHit3D
    {
        simulation::ecs::Entity entity{simulation::ecs::NullEntity};
        Eigen::Vector3d position{};
        Eigen::Vector3d normal{};
        double distance{};
        std::uint32_t triangle{};
    };

    struct MeshQueryWork
    {
        std::size_t tree_nodes{}, candidates{};
    };

    struct MeshQueryStatistics
    {
        // retained_bytes accounts the index object/vector capacities and map
        // value payloads. Map node links/allocator overhead are excluded.
        // geometry_bytes is shared immutable storage, reported separately.
        std::size_t entities{}, geometries{}, tree_nodes{}, retained_bytes{};
        std::uint64_t rebuilds{}, leaf_updates{};
        std::size_t geometry_bytes{};
    };

    class MeshQuerySystem;

    class LUX_ENGINE_SCENE_MESH_QUERY_PUBLIC MeshQuery final
    {
    public:
        MeshQuery();
        ~MeshQuery() noexcept;
        MeshQuery(const MeshQuery&) = delete;
        MeshQuery& operator=(const MeshQuery&) = delete;
        void setGeometry(asset::AssetId, std::shared_ptr<const MeshQueryGeometry>);
        void setGeometryFailure(asset::AssetId, MeshQueryFailure);
        void removeGeometry(asset::AssetId);
        [[nodiscard]] bool hasPendingChanges() const noexcept;
        [[nodiscard]] QueryResult<bool> raycastNearest(
            const math::Ray3d&, double maximum_distance, RayHit3D&, MeshQueryWork* work = nullptr
        ) const;
        [[nodiscard]] MeshQueryStatistics statistics() const noexcept;

    private:
        friend class MeshQuerySystem;
        void structureChanged() noexcept;
        void transformChanged(simulation::ecs::Registry&, simulation::ecs::Entity);
        void update(simulation::ecs::Registry&);
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
