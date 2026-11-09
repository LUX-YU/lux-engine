#pragma once

#include <lux/engine/systems/render/mesh/MeshVisibilitySystem.hpp>
#include <lux/engine/ecs/VisibilityComponent.hpp>
#include <lux/engine/assets/AssetRef.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/world/World.hpp>
#include <lux/engine/world/WorldSpatialIndex.hpp>
#include <entt/container/dense_map.hpp>
#include <entt/container/dense_set.hpp>

#include <array>
#include <optional>
#include <tuple>

namespace lux::engine::render
{
    struct MeshVisibilitySystem::Impl final
    {
        using Bounds = world::WorldSpatialIndex::Bounds;
        using LocalBounds = std::tuple<assets::AssetRef<asset::MeshAsset>, Bounds, std::uint64_t>;
        using Members = entt::dense_map<ecs::Entity, ecs::EVisibilityRequirement>;
        // Prepare, retain, expanded query cache, and cached member/history state (including NONE candidates).
        using View = std::tuple<Bounds, Bounds, Bounds, Members>;
        using AssetObservers = std::pair<
            assets::AssetRef<asset::MeshAsset>, std::pair<entt::scoped_connection, entt::dense_set<ecs::Entity>>
        >;

        world::World& world;
        EngineContext& context;
        world::WorldSpatialIndex& index;
        double prepare_distance;
        double retain_distance;
        double cache_margin;
        double coverage_margin;
        entt::dense_set<ecs::Entity> geometry_changes;
        entt::dense_set<ecs::Entity> processing_geometry;
        entt::dense_set<ecs::Entity> view_changes;
        entt::dense_map<ecs::Entity, LocalBounds> local_bounds;
        entt::dense_set<ecs::Entity> preparing_entities;
        entt::dense_map<asset::AssetId, AssetObservers> asset_observers;
        entt::dense_map<ecs::Entity, View> views;
        entt::dense_map<ecs::Entity, std::array<std::size_t, 2>> relations;
        entt::dense_set<ecs::Entity> dirty_bounds;
        entt::dense_set<ecs::Entity> notifications;
        entt::dense_set<ecs::Entity> candidates;
        std::vector<ecs::Entity> changed_coverage;
        std::vector<ecs::Entity> removed;
        std::vector<ecs::Entity> updated;
        std::vector<Bounds> bounds;
        std::vector<std::size_t> query_offsets;
        std::vector<ecs::Entity> query_objects;
        std::optional<error::Error> failure;
        std::array<entt::scoped_connection, 15> connections;

        Impl(
            world::World& world,
            EngineContext& context,
            std::uint32_t geometry,
            double prepare_distance,
            double retain_distance,
            double cache_margin,
            double coverage_margin
        ) noexcept;
        void
        changeRelation(ecs::Entity entity, ecs::EVisibilityRequirement before, ecs::EVisibilityRequirement after) noexcept;
        void updateMember(View& view, ecs::Entity entity) noexcept;
        void forgetBounds(ecs::Entity entity) noexcept;
        void geometryChanged(ecs::Registry& registry, ecs::Entity entity) noexcept;
        void transformChanged(ecs::Registry& registry, ecs::Entity entity) noexcept;
        void viewChanged(ecs::Registry& registry, ecs::Entity entity) noexcept;
    };
}
