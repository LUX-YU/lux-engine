#pragma once
#include <lux/engine/editor/gui/scene/DocumentPane.hpp>

namespace lux::editor::gui
{
    class ResourcePane final : public DocumentPane<ResourcePane>
    {
      public:
        ResourcePane(scene::SceneEditor &, std::string id);

      private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        std::shared_ptr<const scene::SceneResourceSnapshot> snapshot_;
        bool dirty_{true};
        std::string action_error_;
        object::ScopedConnection resource_connection_;
    };
} // namespace lux::editor::gui
