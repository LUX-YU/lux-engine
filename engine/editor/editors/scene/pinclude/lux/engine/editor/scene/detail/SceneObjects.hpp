#pragma once
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>

namespace lux::editor::scene::detail
{
struct ObjectComponent final
{
    const lux::simulation::ecs::ComponentSchema *schema;
    std::vector<std::byte> bytes;
};

struct AuthorObjectRow final
{
    lux::world::WorldObjectId object, parent;
    std::string label;
    lux::partition::PartitionOrdinal partition;
};

struct ObjectContent final
{
    AuthorObjectRow row;
    std::vector<ObjectComponent> components;
};

inline constexpr editing::HistoryLimits kSceneHistoryLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};

struct SceneObjects final
{
    SceneObjects(lux::scene::SceneInstance &scene, const NativeScene &source,
                 const lux::simulation::ecs::ComponentSchemaSet &metadata);

    lux::simulation::ecs::Registry &registry;
    const NativeScene &source;
    const lux::simulation::ecs::ComponentSchemaSet &metadata;
    lux::scene::WorldLoadingSystem &loading;
    lux::simulation::ecs::WorldEntityMap &identities;
    lux::scene::SceneInstanceId instance;
    std::vector<SceneObjectRow> rows;
    std::vector<ComponentNotice> component_versions;
    SelectionNotice selection;
    editing::Revision structure_revision{};
    std::uint64_t next_component_change{1};
    bool structural_commit{};
    bool structure_changed{};
    bool invalidate_derived{};

    [[nodiscard]] SceneEntityRef reference(lux::simulation::ecs::Entity entity) const noexcept
    {
        return {instance, entity};
    }
    [[nodiscard]] SceneEntityRef authorReference(lux::world::WorldObjectId id) const noexcept
    {
        return reference(identities.entity(id));
    }
    [[nodiscard]] lux::simulation::ecs::Entity resolve(SceneEntityRef value) const noexcept
    {
        return value.instance == instance && registry.valid(value.entity) ? value.entity
                                                                          : lux::simulation::ecs::NullEntity;
    }
    [[nodiscard]] lux::world::WorldObjectId persistent(SceneEntityRef value) const noexcept
    {
        return identities.object(resolve(value));
    }

    static bool componentLess(const ComponentNotice &, const ComponentNotice &) noexcept;
    [[nodiscard]] std::vector<lux::scene::PartitionRetention> retainTargets(lux::simulation::ecs::Entity,
                                                                            lux::cxx::TypeToken) const;
    editing::EditResult<ObjectContent> captureObject(SceneObjectRow row, const lux::simulation::ecs::Registry &registry,
                                                     const lux::simulation::ecs::WorldEntityMap &mapping) const;

    template <class Component>
    editing::EditResult<ObjectComponent> encodeComponent(const Component &value,
                                                         const lux::simulation::ecs::WorldEntityMap &mapping) const
    {
        const auto *schema = metadata.find(lux::cxx::typeToken<Component>());
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
                static_cast<std::uint64_t>(ESceneStructureError::CODEC_FAILURE), schema->id.name));
        }
        return ObjectComponent{schema, std::move(*encoded)};
    }
};
} // namespace lux::editor::scene::detail
