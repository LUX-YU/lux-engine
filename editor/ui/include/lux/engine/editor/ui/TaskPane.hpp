#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::editor
{
    class EditorContext;
}

namespace lux::editor::ui
{
    // A view of Runtime facts. Closing the window does not stop tasks.
    class LUX_EDITOR_UI_PUBLIC TaskPane final : public lux::ui::Pane
    {
    public:
        TaskPane(lux::ui::Root&, EditorContext&, EditorResult<void>&);
        ~TaskPane() noexcept override;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
