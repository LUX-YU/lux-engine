#pragma once
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/editor/scene/detail/SceneOpening.hpp>
#include <lux/engine/editor/editing/scene/SceneEdit.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/scene/WorldResidency.hpp>

namespace lux::editor::scene::detail
{
    struct ObjectComponent final
    {
        const lux::simulation::ecs::ComponentSchema* schema;
        std::vector<std::byte> bytes;
    };

    struct ObjectContent final
    {
        lux::world::WorldObjectId object;
        lux::partition::PartitionOrdinal partition;
        std::vector<ObjectComponent> components;
    };

    inline constexpr editing::HistoryLimits kSceneHistoryLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};

    struct SceneContent final
    {
        SceneContent(
            lux::scene::SceneRuntime& runtime,
            lux::scene::SceneInstanceId scene,
            const lux::scene::ScenePackage& source,
            const lux::simulation::ecs::ComponentSchemaSet& metadata
        );

        lux::scene::SceneRuntime& runtime;
        [[nodiscard]] lux::simulation::ecs::Registry& registry() const noexcept
        {
            auto borrowed = runtime.borrowInstance(instance);
            if (!borrowed)
                std::terminate();
            return borrowed->get();
        }
        [[nodiscard]] const lux::simulation::ecs::Registry& readRegistry() const noexcept
        {
            auto borrowed = std::as_const(runtime).borrowInstance(instance);
            if (!borrowed)
                std::terminate();
            return borrowed->get();
        }
        [[nodiscard]] lux::scene::WorldResidency& residency() const noexcept
        {
            return registry().ctx().get<lux::scene::WorldResidency>();
        }
        [[nodiscard]] const lux::simulation::ecs::WorldEntityMap& identities() const noexcept
        {
            return readRegistry().ctx().get<lux::scene::WorldResidency>().identities();
        }
        const lux::simulation::ecs::ComponentSchemaSet& metadata;
        const lux::scene::SceneInstanceId instance;
        [[nodiscard]] lux::simulation::ecs::Entity resolve(lux::simulation::ecs::Entity entity) const noexcept
        {
            return readRegistry().valid(entity) ? entity : lux::simulation::ecs::NullEntity;
        }
        const lux::scene::ScenePackage& source;
        bool structural_commit{};
        bool structure_changed{};

        [[nodiscard]] lux::world::WorldObjectId persistent(lux::simulation::ecs::Entity value) const noexcept
        {
            return identities().object(resolve(value));
        }

        editing::EditResult<ObjectContent> captureObject(lux::simulation::ecs::Entity entity) const;

        template <class Component>
        editing::EditResult<ObjectComponent> encodeComponent(
            const Component& value,
            const lux::simulation::ecs::WorldEntityMap& mapping
        ) const
        {
            auto encoded = encodeSceneValue(value, metadata, mapping, kSceneHistoryLimits.max_staging_bytes);
            if (!encoded)
                return lux::cxx::unexpected(encoded.error());
            return ObjectComponent{metadata.find(encoded->schema), std::move(encoded->bytes)};
        }
    };
} // namespace lux::editor::scene::detail
