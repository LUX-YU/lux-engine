#pragma once

#include <lux/engine/editor/ui/ComponentEditors.hpp>
#include <lux/engine/editor/ui/InspectorInteraction.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace consumer
{
    // Consumer-owned host for one generated component factory. No scene-private Inspector dependency.
    class ComponentFactoryPane final : public lux::ui::Pane
    {
    public:
        ComponentFactoryPane(lux::ui::Pane& parent,
                             lux::ui::PaneId id,
                             lux::editor::scene::SceneEditing& editing,
                             lux::editor::ComponentEditorRegistration binding)
            : lux::ui::Pane(parent, std::move(id), lux::ui::PaneTypeId{"consumer.component"}, "Component"),
              binding_(std::move(binding)), editing_(&editing), interaction_(editing, "consumer"),
              layout_(*this, lux::ui::ElementId{"content"})
        {
            setContent(layout_);
        }

        lux::editor::EditorResult<void> setTarget(lux::editor::scene::SceneEditing& editing,
                                                 lux::simulation::ecs::Entity target)
        {
            auto finished = finishEditing();
            if (!finished)
                return finished;
            fields_.reset();
            editing_ = &editing;
            interaction_.bind(editing);
            if (target == lux::simulation::ecs::NullEntity)
                return {};
            auto created = binding_.create(layout_, lux::ui::ElementId{binding_.name}, editing, target, interaction_);
            if (!created)
                return lux::cxx::unexpected(created.error());
            fields_ = std::move(*created);
            return {};
        }

        lux::editor::EditorResult<void> finishEditing()
        {
            const auto finish = [&](auto&& self, lux::ui::Element& element) -> void {
                element.finishEdit();
                for (auto* child = element.firstChild(); child; child = child->nextSibling())
                    self(self, *static_cast<lux::ui::Element*>(child));
            };
            finish(finish, layout_);
            if (interaction_.finish() && interaction_.finishPending())
                return {};
            return lux::cxx::unexpected(lux::editor::EditorFailure{
                lux::editor::EEditorError::INVALID_STATE, "consumer.component", 0, {}, interaction_.failure()
            });
        }

        void requestClose() noexcept
        {
            hide_requested_ = true;
        }

    private:
        void update() noexcept override
        {
            if (hide_requested_ && finishEditing())
            {
                setVisible(false);
                hide_requested_ = false;
            }
            if (!interaction_.active())
                static_cast<void>(interaction_.finishPending());
        }

        // The record must outlive destruction of the factory's polymorphic result.
        lux::editor::ComponentEditorRegistration binding_;
        lux::editor::scene::SceneEditing* editing_;
        lux::editor::ui::InspectorInteraction interaction_;
        lux::ui::Layout layout_;
        std::unique_ptr<lux::ui::Element> fields_;
        bool hide_requested_{};
    };
}
