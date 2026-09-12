#pragma once
#include <lux/engine/editor/ui/actions/HistoryActions.hpp>
#include <lux/engine/ui/CommandRouter.hpp>

namespace lux::editor::ui::detail
{
    class EditorEditMenu final
    {
    public:
        EditorEditMenu(lux::object::ObjectDispatcherRef, ActiveEditHistory &, lux::ui::CommandRouter &);
        [[nodiscard]] bool valid() const noexcept { return valid_; }
        void draw();
    private:
        HistoryMenuActions actions_;
        lux::ui::CommandRouter &commands_;
        lux::ui::CommandHandle undo_, redo_;
        lux::ui::CommandRegistration undo_binding_, redo_binding_;
        bool valid_{}, open_{};
    };
}
