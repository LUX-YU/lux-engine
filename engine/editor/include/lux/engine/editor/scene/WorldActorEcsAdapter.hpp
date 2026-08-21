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
    /// Editor-only bridge between reflected ECS proxies and generic LXAD.
    /// Authoring codecs stay ECS-free; Runtime never links this adapter.
    class WorldActorEcsAdapter final
    {
    public:
        explicit WorldActorEcsAdapter(
            const lux::ecs::ComponentTypeCatalog& components,
            lux::ecs::PersistentEntityIndex& persistent_entities) noexcept
            : components_(components),
              persistent_entities_(persistent_entities)
        {}

        [[nodiscard]] lux::cxx::expected<
            lux::authoring::WorldActorDocument,
            std::string>
        capture(
            lux::ecs::Registry& registry,
            entt::entity entity,
            lux::authoring::WorldId world,
            std::string_view origin = "LXAD Actor");

        [[nodiscard]] lux::cxx::expected<entt::entity, std::string>
        materialize(
            const lux::authoring::WorldActorDocument& document,
            lux::ecs::Registry& registry,
            std::string_view origin = "LXAD Actor") const;

    private:
        const lux::ecs::ComponentTypeCatalog& components_;
        lux::ecs::PersistentEntityIndex& persistent_entities_;
    };
} // namespace lux::editor
