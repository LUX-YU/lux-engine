#pragma once

#include <lux/engine/scene/world_materialization/visibility.h>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <lux/engine/world/WorldDescription.hpp>
#include <lux/engine/world/WorldPartitionData.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace lux::scene
{
    enum class EWorldMaterializeError : std::uint8_t
    {
        INVALID_WORLD_SCHEMA,
        INVALID_OBJECT,
        COMPONENT_DECODE_FAILURE,
        ALLOCATION_FAILURE,
        DUPLICATE_OBJECT,
        CAPACITY,
        STRUCTURE_CHANGED,
    };

    struct WorldMaterializeFailure final
    {
        EWorldMaterializeError code{EWorldMaterializeError::INVALID_OBJECT};
        simulation::ecs::ComponentDecodeFailure component;
        std::size_t object{};
        std::size_t data{};
    };

    struct WorldComponentInput final
    {
        const simulation::ecs::ComponentSchema* schema{};
        std::uint32_t version{};
        std::span<const std::byte> payload;
    };

    struct WorldObjectInput final
    {
        world::WorldObjectId id;
        std::span<const WorldComponentInput> components;
    };

    class LUX_ENGINE_SCENE_WORLD_MATERIALIZATION_PUBLIC WorldMaterializer final
    {
    public:
        class LUX_ENGINE_SCENE_WORLD_MATERIALIZATION_PUBLIC PreparedObjects final
        {
        public:
            ~PreparedObjects() noexcept;
            PreparedObjects(PreparedObjects&&) noexcept;
            PreparedObjects& operator=(PreparedObjects&&) noexcept;
            [[nodiscard]] std::span<const simulation::ecs::Entity> entities() const noexcept;
            [[nodiscard]] std::span<const simulation::ecs::Entity> references() const noexcept;
            [[nodiscard]] simulation::ecs::Entity entity(world::WorldObjectId) const noexcept;
            [[nodiscard]] std::size_t componentBytes() const noexcept;
            // Same structural exclusion as prepare; no normal business rejection after mutation starts.
            void commit(simulation::ecs::Registry&, simulation::ecs::WorldEntityMap&) noexcept;

        private:
            friend class WorldMaterializer;
            struct Impl;
            explicit PreparedObjects(std::unique_ptr<Impl>) noexcept;
            std::unique_ptr<Impl> impl_;
        };

        [[nodiscard]] static lux::cxx::expected<PreparedObjects, WorldMaterializeFailure> prepareObjects(
            const simulation::ecs::Registry&,
            const simulation::ecs::WorldEntityMap&,
            std::span<const WorldObjectInput>,
            std::size_t maximum_component_bytes = (std::numeric_limits<std::size_t>::max)()
        ) noexcept;

        [[nodiscard]] static lux::cxx::expected<WorldMaterializer, WorldMaterializeFailure> create(
            std::shared_ptr<const world::WorldDescription> world,
            simulation::ecs::ComponentSchemaSet components
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<simulation::ecs::Entity, WorldMaterializeFailure> object(
            simulation::ecs::Registry& registry,
            simulation::ecs::WorldEntityMap& identities,
            world::WorldPartitionObjectView object
        ) const noexcept;

        // One Registry/identity index at its structural safe point. Resolves the complete incoming
        // partition before decode; failures retain resident content, source data and the created output.
        [[nodiscard]] lux::cxx::expected<void, WorldMaterializeFailure> partition(
            simulation::ecs::Registry& registry,
            simulation::ecs::WorldEntityMap& identities,
            const world::WorldPartitionData& data,
            std::vector<simulation::ecs::Entity>* created = nullptr
        ) const noexcept;

        // Decode the complete batch before Registry mutation; resolve references against resident
        // identities and the planned Entity set. Unknown source payload stays with its source owner.
        // The byte budget counts inline decoded values, not provider internal allocations.
        [[nodiscard]] lux::cxx::expected<void, WorldMaterializeFailure> objects(
            simulation::ecs::Registry& registry,
            simulation::ecs::WorldEntityMap& identities,
            std::span<const world::WorldPartitionObjectView> input,
            std::vector<simulation::ecs::Entity>* created = nullptr,
            std::size_t maximum_component_bytes = (std::numeric_limits<std::size_t>::max)(),
            std::size_t* component_bytes = nullptr
        ) const noexcept;

    private:
        WorldMaterializer(
            std::shared_ptr<const world::WorldDescription> world,
            simulation::ecs::ComponentSchemaSet components,
            std::vector<const simulation::ecs::ComponentSchema*> mappings
        ) noexcept;

        std::shared_ptr<const world::WorldDescription> world_;
        simulation::ecs::ComponentSchemaSet components_;
        std::vector<const simulation::ecs::ComponentSchema*> mappings_;
    };
} // namespace lux::scene
