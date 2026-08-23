#include <lux/engine/ui/Pane.hpp>

#include <algorithm>
#include <utility>

namespace lux::ui {
void PaneDrawContext::activateContext(UiContextIdView context) {
  if (!context.isValid())
    return;
  std::erase(*active_, context);
  active_->push_back(context);
}

Pane::Pane(lux::object::ObjectDispatcherRef dispatcher, PaneId id,
           PaneTypeId type, std::string title)
    : Object(std::move(dispatcher)), id_(std::move(id)), type_(std::move(type)),
      title_(std::move(title)) {
  rebuildImguiLabel();
}

Pane::~Pane() = default;

void Pane::setTitle(std::string title) {
  if (title_ == title)
    return;
  title_ = std::move(title);
  rebuildImguiLabel();
}

void Pane::setVisible(bool visible) {
  if (visible_ == visible)
    return;
  visible_ = visible;
  notify<visibilityChanged>(PaneVisibilityChanged{visible_});
}

void Pane::setFocused(bool focused) {
  if (focused_ == focused)
    return;
  focused_ = focused;
  notify<focusChanged>(PaneFocusChanged{focused_});
}

void Pane::rebuildImguiLabel() {
  imgui_label_ = title_;
  imgui_label_ += "###";
  imgui_label_ += id_.name();
}
} // namespace lux::ui
