#pragma once

#include <lux/engine/math/MeshBVH.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
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

    class LUX_ENGINE_SCENE_MESH_QUERY_PUBLIC MeshQuerySystem final
    {
      public:
        inline static constexpr std::array Capabilities{std::string_view{"lux.scene.mesh_query.3d"}};
        inline static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.builtin.system.mesh_query.3d",
            .version = 1U,
            .capabilities = Capabilities,
            .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER};

        explicit MeshQuerySystem(simulation::ecs::Registry &registry);
        ~MeshQuerySystem() noexcept;
        MeshQuerySystem(const MeshQuerySystem &) = delete;
        MeshQuerySystem &operator=(const MeshQuerySystem &) = delete;

        // Main adopts a complete asset version. Removing/replacing a value
        // invalidates the index; queries never perform IO or build a BVH.
        void setGeometry(asset::AssetId source, std::shared_ptr<const MeshQueryGeometry> geometry);
        void setGeometryFailure(asset::AssetId source, MeshQueryFailure failure);
        void removeGeometry(asset::AssetId source);
        void updateStablePoint();
        [[nodiscard]] bool hasPendingChanges() const noexcept;

        [[nodiscard]] QueryResult<bool> raycastNearest(const math::Ray3d &ray, double maximum_distance, RayHit3D &hit,
                                                       MeshQueryWork *work = nullptr) const;
        [[nodiscard]] MeshQueryStatistics statistics() const noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_MESH_QUERY_PUBLIC SceneSystemRegistration
    builtinMeshQuerySystemRegistration() noexcept;
} // namespace lux::scene
