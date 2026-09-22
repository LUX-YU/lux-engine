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

class LUX_ENGINE_SCENE_WORLD_MATERIALIZATION_PUBLIC WorldMaterializer final
{
  public:
    [[nodiscard]] static lux::cxx::expected<WorldMaterializer, WorldMaterializeFailure> create(
        std::shared_ptr<const world::WorldDescription> world, simulation::ecs::ComponentSchemaSet components) noexcept;

    [[nodiscard]] lux::cxx::expected<simulation::ecs::Entity, WorldMaterializeFailure> object(
        simulation::ecs::Registry &registry, simulation::ecs::WorldEntityMap &identities,
        world::WorldPartitionObjectView object) const noexcept;

    // One Registry/identity index at its structural safe point. Resolves the complete incoming
    // partition before decode; failures retain resident content, source data and the created output.
    [[nodiscard]] lux::cxx::expected<void, WorldMaterializeFailure> partition(
        simulation::ecs::Registry &registry, simulation::ecs::WorldEntityMap &identities,
        const world::WorldPartitionData &data, std::vector<simulation::ecs::Entity> *created = nullptr) const noexcept;

    // Decode the complete batch before Registry mutation; resolve references against resident
    // identities and the planned Entity set. Unknown source payload stays with its source owner.
    // The byte budget counts inline decoded values, not provider internal allocations.
    [[nodiscard]] lux::cxx::expected<void, WorldMaterializeFailure> objects(
        simulation::ecs::Registry &registry, simulation::ecs::WorldEntityMap &identities,
        std::span<const world::WorldPartitionObjectView> input, std::vector<simulation::ecs::Entity> *created = nullptr,
        std::size_t maximum_component_bytes = (std::numeric_limits<std::size_t>::max)(),
        std::size_t *component_bytes = nullptr) const noexcept;

  private:
    WorldMaterializer(std::shared_ptr<const world::WorldDescription> world,
                      simulation::ecs::ComponentSchemaSet components,
                      std::vector<const simulation::ecs::ComponentSchema *> mappings) noexcept;

    std::shared_ptr<const world::WorldDescription> world_;
    simulation::ecs::ComponentSchemaSet components_;
    std::vector<const simulation::ecs::ComponentSchema *> mappings_;
};
} // namespace lux::scene
