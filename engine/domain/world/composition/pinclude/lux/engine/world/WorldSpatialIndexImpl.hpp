#pragma once

#include <lux/engine/world/WorldSpatialIndex.hpp>
#include <entt/container/dense_map.hpp>
#include <cstdint>

namespace lux::world
{
    struct WorldSpatialIndex::Impl final
    {
        struct Record final
        {
            ecs::Entity entity;
            Bounds actual_bounds;
            Bounds coverage_bounds;
        };

        static constexpr std::uint32_t absent = UINT32_MAX;
        std::uint32_t root = absent;
        entt::dense_map<ecs::Entity, std::uint32_t> leaves;
        std::vector<Record> records;
        std::vector<Bounds> node_bounds;
        std::vector<std::uint32_t> parents;
        std::vector<std::uint32_t> left;
        std::vector<std::uint32_t> right;
        std::vector<std::uint32_t> record_indices;
        std::vector<std::uint32_t> heights;
        std::vector<std::uint32_t> free_nodes;
        // Reused validation batch: input offset and stable leaf index; no iterator survives map growth.
        entt::dense_map<ecs::Entity, std::pair<std::size_t, std::uint32_t>> input_entities;
        mutable std::vector<std::uint32_t> query_stack;

        std::uint32_t allocateNode() noexcept;
        void releaseNode(std::uint32_t node) noexcept;
        void refit(std::uint32_t node) noexcept;
        std::uint32_t balance(std::uint32_t node) noexcept;
        void insert(std::uint32_t leaf) noexcept;
        void detach(std::uint32_t leaf) noexcept;
    };
}
