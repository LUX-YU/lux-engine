#pragma once

#include <lux/engine/authoring/world/WorldSource.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <entt/entity/entity.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lux::ecs
{
    class ComponentTypeCatalog;
    class PersistentEntityIndex;
    class Registry;
}

namespace lux::editor
{
    /// One-way Editor conversion from reflected ECS state to an LXAD Actor.
    /// Authoring codecs remain ECS-free and no long-lived synchronization
    /// object is created at this representation boundary.
    [[nodiscard]] lux::cxx::expected<
        lux::authoring::WorldActorDocument,
        std::string>
    serializeWorldActor(
        const lux::ecs::ComponentTypeCatalog& components,
        lux::ecs::PersistentEntityIndex& persistent_entities,
        lux::ecs::Registry& registry,
        entt::entity entity,
        lux::authoring::WorldId world,
        std::string_view origin = "LXAD Actor");

    /// One-way Editor conversion from an LXAD Actor to reflected ECS state.
    [[nodiscard]] lux::cxx::expected<entt::entity, std::string>
    materializeWorldActor(
        const lux::ecs::ComponentTypeCatalog& components,
        lux::ecs::PersistentEntityIndex& persistent_entities,
        const lux::authoring::WorldActorDocument& document,
        lux::ecs::Registry& registry,
        std::string_view origin = "LXAD Actor");
} // namespace lux::editor
