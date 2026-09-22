#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/partition/PartitionOrdinal.hpp>
#include <lux/engine/scene/world_loading/visibility.h>
#include <lux/engine/simulation/ecs/ComponentAnnotations.hpp>
#include <lux/engine/simulation/ecs/ComponentSchema.hpp>
#include <vector>

namespace lux::scene
{
// Explicit complete demand, including dependency partitions. No Camera or spatial index.
// Mutations use Registry::patch so the loading system can coalesce demand changes.
struct LUX_COMPONENT(schema = "lux.scene.Observer", version = 1, snapshot = COPY, semantic = DOMAIN_CONTRACT,
                     editor = true) Observer final
{
    std::vector<partition::PartitionOrdinal> LUX_MEMBER(display_name = Partitions) partitions;
    bool LUX_MEMBER(display_name = Required) required{true};
};

[[nodiscard]] LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC std::span<const simulation::ecs::ComponentSchema>
worldLoadingComponentSchemas() noexcept;
} // namespace lux::scene

#if !defined(__LUX_PARSE_TIME__)
#include <lux/engine/scene/Observer.type_static_info.hpp>
#endif
