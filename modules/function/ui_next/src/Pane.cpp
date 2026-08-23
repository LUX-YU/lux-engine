#include <lux/engine/ui_next/Pane.hpp>

#include <algorithm>
#include <utility>

namespace lux::ui
{
    void PaneDrawContext::activateContext(UiContextId context)
    {
        if (!context.isValid()) return;
        std::erase(*active_, context);
        active_->push_back(std::move(context));
    }

    Pane::Pane(
        lux::object::ObjectDispatcher& dispatcher,
        PaneId id,
        PaneTypeId type,
        std::string title
    )
        : id_(std::move(id)),
          type_(std::move(type)),
          title_(std::move(title))
    {
        setDispatcher(&dispatcher);
    }

    Pane::~Pane() = default;

    void Pane::setVisible(bool visible)
    {
        if (visible_ == visible) return;
        visible_ = visible;
        emit(visibilityChanged, PaneVisibilityChanged{visible_});
    }

    void Pane::setFocused(bool focused)
    {
        if (focused_ == focused) return;
        focused_ = focused;
        emit(focusChanged, PaneFocusChanged{focused_});
    }
}
