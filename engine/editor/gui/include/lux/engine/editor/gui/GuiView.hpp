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

        virtual void appendFrameImages(std::vector<rendering::ViewImage> &) const {}

        virtual void releaseFrameImages() noexcept {}
    };
} // namespace lux::editor::gui
