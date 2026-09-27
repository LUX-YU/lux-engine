#pragma once
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
        lux::world::WorldObjectId object, parent;
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
            auto borrowed = runtime.getSceneRegistry(instance);
            if (!borrowed)
                std::terminate();
            return borrowed->get();
        }
        [[nodiscard]] const lux::simulation::ecs::Registry& readRegistry() const noexcept
        {
            auto borrowed = std::as_const(runtime).getSceneRegistry(instance);
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
            const auto* schema = metadata.find(lux::cxx::typeToken<Component>());
            if (!schema || !schema->decode_value || !schema->capture_value)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::UNSUPPORTED_OPERATION));
            }
            auto capture = schema->capture_value(&value, schema->code_lifetime);
            auto encoded = capture.encode(mapping, kSceneHistoryLimits.max_staging_bytes);
            if (!encoded)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(
                    editing::EEditError::PRECONDITION_FAILED,
                    static_cast<std::uint64_t>(ESceneStructureError::CODEC_FAILURE),
                    schema->id.name
                ));
            }
            return ObjectComponent{schema, std::move(*encoded)};
        }
    };
} // namespace lux::editor::scene::detail
