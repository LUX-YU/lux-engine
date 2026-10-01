#pragma once

#include <lux/engine/editor/metadata/ComponentEditorRegistry.hpp>
#include <lux/engine/editor/ui/InspectorInteraction.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::ui
{
    class InspectorElement final : public lux::ui::Element
    {
    public:
        InspectorElement(
            lux::ui::Pane& parent,
            lux::ui::ElementId id,
            const lux::simulation::ecs::ComponentSchemaSet& schemas,
            const ComponentEditorRegistry& editors,
            EditorResult<void>& status,
            project::ProjectCatalogModel* catalog
        );
        [[nodiscard]] EditorResult<void> setTarget(scene::SceneEditing&, lux::simulation::ecs::Entity);
        [[nodiscard]] EditorResult<void> finishEditing();
        [[nodiscard]] EditorResult<void> clearTarget();

    private:
        struct ComponentRow final
        {
            lux::cxx::TypeToken type;
            // Code outlives both virtual destruction and any connection teardown.
            ComponentEditorRegistration binding;
            std::unique_ptr<lux::ui::Label> title;
            std::unique_ptr<lux::ui::Element> content;
        };
        void update() noexcept override;
        [[nodiscard]] bool sameComponents() const noexcept;
        void rebuild();
        const ComponentEditorRegistry& editors_;
        project::ProjectCatalogModel* catalog_;
        scene::SceneEditing* editing_{};
        lux::simulation::ecs::Entity target_{lux::simulation::ecs::NullEntity};
        std::optional<InspectorInteraction> interaction_;
        lux::ui::Layout layout_;
        lux::ui::Label message_;
        std::vector<lux::cxx::TypeToken> creation_types_;
        lux::ui::Layout creation_layout_;
        lux::ui::Choice creation_choice_;
        lux::ui::Button add_component_;
        std::optional<std::pair<lux::simulation::ecs::Entity, lux::cxx::TypeToken>> add_requested_;
        object::Connection add_connection_;
        std::vector<ComponentRow> components_;
        bool dirty_{true};
        lux::ui::SizeHint sizeHintContent() noexcept override;
        lux::ui::SizeHint measureContent(float width) noexcept override;
        void arrangeContent() noexcept override;
        void draw() noexcept override;
    };
}
