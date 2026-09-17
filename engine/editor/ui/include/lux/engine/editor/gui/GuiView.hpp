#pragma once

#include <lux/engine/editor/DocumentEditor.hpp>
#include <lux/engine/editor/rendering/ViewImage.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::gui
{
    class GuiView : public DocumentView
    {
      public:
        virtual lux::ui::Pane &pane() noexcept = 0;

        // Called by the frontend at an owner boundary before save/close review.
        [[nodiscard]] virtual EditorResult<void> finishInteraction()
        {
            return {};
        }

        // References used by the current draw survive visibility changes until
        // seal/releaseFrameImages; visibility is not a record of what was drawn.
        virtual void appendFrameImages(std::vector<rendering::ViewImage> &) const {}

        virtual void releaseFrameImages() noexcept {}
    };
} // namespace lux::editor::gui
