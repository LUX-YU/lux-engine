#pragma once

#include <lux/engine/scene/runtime/world/visibility.h>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/world/WorldDescription.hpp>
#include <lux/engine/world/WorldPartitionData.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
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
    };

    struct WorldMaterializeFailure final
    {
        EWorldMaterializeError code{EWorldMaterializeError::INVALID_OBJECT};
        simulation::ecs::ComponentDecodeFailure component;
        std::size_t object{};
        std::size_t data{};
    };

    class LUX_ENGINE_SCENE_WORLD_RUNTIME_PUBLIC WorldMaterializer final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<WorldMaterializer, WorldMaterializeFailure> create(
            std::shared_ptr<const world::WorldDescription> world,
            simulation::ecs::ComponentSchemaSet components
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<simulation::ecs::Entity, WorldMaterializeFailure> object(
            simulation::ecs::Registry& registry,
            world::WorldPartitionObjectView object
        ) const noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldMaterializeFailure> partition(
            simulation::ecs::Registry& registry,
            const world::WorldPartitionData& data,
            std::vector<simulation::ecs::Entity>* created = nullptr
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
}
