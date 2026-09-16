#pragma once
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/scene/SceneMetaManager.hpp>

namespace lux::editor::scene::detail
{
    struct ObjectComponent final
    {
        const lux::simulation::ecs::ComponentSchema *schema;
        std::vector<std::byte> bytes;
    };

    struct ObjectContent final
    {
        SceneObjectRow row;
        std::vector<ObjectComponent> components;
    };

    inline constexpr editing::HistoryLimits kSceneHistoryLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};

    struct SceneObjects final
    {
        SceneObjects(lux::simulation::ecs::Registry &registry, const NativeScene &source,
                     const lux::scene::SceneMetaManager &metadata, lux::simulation::ecs::WorldEntityMap identities);

        lux::simulation::ecs::Registry &registry;
        const NativeScene &source;
        const lux::scene::SceneMetaManager &metadata;
        lux::simulation::ecs::WorldEntityMap identities;
        std::vector<SceneObjectRow> rows;
        std::vector<ComponentNotice> component_versions;
        SelectionNotice selection;
        std::uint64_t next_component_change{1};
        bool structural_commit{};
        bool structure_changed{};
        bool invalidate_derived{};

        static bool componentLess(const ComponentNotice &, const ComponentNotice &) noexcept;
        editing::EditResult<ObjectContent> captureObject(SceneObjectRow row,
                                                         const lux::simulation::ecs::Registry &registry,
                                                         const lux::simulation::ecs::WorldEntityMap &mapping) const;

        template <class Component>
        editing::EditResult<ObjectComponent> encodeComponent(const Component &value,
                                                             const lux::simulation::ecs::WorldEntityMap &mapping) const
        {
            const auto *schema = metadata.getComponentMeta(lux::cxx::typeToken<Component>());
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
