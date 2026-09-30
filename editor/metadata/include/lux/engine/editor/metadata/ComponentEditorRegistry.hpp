#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/cxx/core/StableNameId.hpp>
#include <lux/engine/editor/metadata/visibility.h>
#include <lux/engine/project/PluginCatalog.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>

namespace lux::ui
{
    class Element;
    struct ElementIdTag;
}
namespace lux::editor::scene
{
    class SceneEditing;
}
namespace lux::editor::ui
{
    class InspectorInteraction;
}

namespace lux::editor
{
    struct ComponentEditorRegistration final
    {
        using CreateResult = EditorResult<std::unique_ptr<lux::ui::Element>>;
        using CreateFn =
            CreateResult (*)(
                lux::ui::Element&,
                lux::cxx::StableNameId<lux::ui::ElementIdTag>,
                scene::SceneEditing&,
                lux::simulation::ecs::Entity,
                ui::InspectorInteraction&
            ) noexcept;
        lux::cxx::TypeToken type;
        std::string name;
        CreateFn create{};
        std::shared_ptr<const void> code_lifetime;
        lux::project::MetadataIdentity provider;
    };

    // Immutable editor extensions. Runtime component semantics remain in the schema set.
    class LUX_EDITOR_METADATA_PUBLIC ComponentEditorRegistry final
    {
    public:
        ComponentEditorRegistry() noexcept = default;
        [[nodiscard]] static EditorResult<ComponentEditorRegistry> create(
            simulation::ecs::ComponentSchemaSet schemas,
            std::vector<ComponentEditorRegistration> registrations
        ) noexcept;

        [[nodiscard]] const ComponentEditorRegistration* find(lux::cxx::TypeToken type) const noexcept;
        [[nodiscard]] std::span<const ComponentEditorRegistration> all() const noexcept;

    private:
        struct Impl;
        explicit ComponentEditorRegistry(std::shared_ptr<const Impl> impl) noexcept;
        std::shared_ptr<const Impl> impl_;
    };
}
