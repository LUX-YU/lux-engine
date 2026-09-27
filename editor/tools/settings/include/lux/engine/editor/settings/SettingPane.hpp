#pragma once
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <memory>
namespace lux::editor
{
    class EditorContext;
    class SettingPane final : public lux::ui::Pane
    {
    public:
        SettingPane(lux::ui::Root&, EditorContext&, EditorResult<void>&);
        ~SettingPane() override;

    private:
        void update() noexcept override;
        class Content;
        std::unique_ptr<Content> content_;
        object::Connection close_connection_;
        bool hide_requested_{};
    };
}
