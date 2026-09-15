#pragma once
#include <lux/engine/editor/gui/DocumentPane.hpp>
#include <lux/engine/editor/gui/scene/ComponentBinding.hpp>
#include <lux/engine/editor/gui/scene/InspectorInteraction.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>

namespace lux::editor::gui
{
    class InspectorPane final : public DocumentPane<InspectorPane, scene::SceneEditor>
    {
      public:
        InspectorPane(scene::SceneEditor &, std::string id, std::shared_ptr<const std::vector<ComponentBinding>>);
        void requestClose() noexcept override;
        void poll(PollBudget &) override;
        [[nodiscard]] EditorResult<void> finishInteraction() override;

      private:
        struct ComponentRow final
        {
            scene::SceneComponentInfo info;
            const ComponentBinding *binding{};
            bool open{true};
        };

        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        std::shared_ptr<const std::vector<ComponentBinding>> bindings_;
        InspectorInteraction interaction_;
        std::vector<ComponentRow> components_;
        scene::SelectionNotice selection_;
        bool directory_dirty_{true};
        object::ScopedConnection selection_connection_;
        object::ScopedConnection objects_connection_;
    };
} // namespace lux::editor::gui
