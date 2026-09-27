#pragma once
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Pane.hpp>
namespace lux::editor
{
    class ProjectPane final : public lux::ui::Pane
    {
    public:
        ProjectPane(lux::ui::Root&, EditorContext&, EditorResult<void>&);

    private:
        class Content final : public lux::ui::Element
        {
        public:
            explicit Content(ProjectPane& owner) : Element(owner, lux::ui::ElementId{"content"}), owner_(owner) {}

        private:
            void draw() noexcept override;
            ProjectPane& owner_;
        };
        void drawActions() noexcept;
        void update() noexcept override;
        Content content_;
        EditorContext& context_;
        object::Connection close_connection_;
        bool hide_requested_{};
    };
}
