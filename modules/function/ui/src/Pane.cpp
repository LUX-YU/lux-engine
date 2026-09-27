#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/detail/Contract.hpp>

#include <algorithm>
#include <utility>

namespace lux::ui
{
    Pane::Pane(Root& parent, PaneId id, PaneTypeId type, std::string title)
        : Pane(parent, parent, std::move(id), std::move(type), std::move(title))
    {}

    Pane::Pane(Pane& parent, PaneId id, PaneTypeId type, std::string title)
        : Pane(parent, parent.root(), std::move(id), std::move(type), std::move(title))
    {}

    Pane::Pane(object::LuxObject& parent, Root& root, PaneId id, PaneTypeId type, std::string title)
        : LuxObject(parent.dispatcherRef()), id_(std::move(id)), type_(std::move(type)), title_(std::move(title)),
          root_(&root)
    {
        if (!id_.isValid())
            detail::failContract();
        root.checkContentChange();
        rebuildWindowLabel();
        attachTo(parent);
        root.registerPane(*this);
    }

    Pane::~Pane()
    {
        root().unregisterPane(*this);
    }

    Root& Pane::root() const noexcept
    {
        if (!root_)
            detail::failContract();
        return *root_;
    }

    void Pane::setContent(Element& element) noexcept
    {
        root().checkContentChange();
        if (!isOnAffinityThread() || element.parent() != this)
            detail::failContract();
        content_ = &element;
    }

    void Pane::setTitle(std::string title)
    {
        if (title_ == title)
            return;
        title_ = std::move(title);
        rebuildWindowLabel();
        root().paneLabelChanged();
    }

    void Pane::setVisible(bool visible)
    {
        if (visible_ == visible)
            return;
        visible_ = visible;
        static_cast<void>(emit(visibilityChanged, PaneVisibilityChanged{visible_}));
    }

    void Pane::setModal(bool modal) noexcept
    {
        root().checkContentChange();
        modal_ = modal;
    }

    void Pane::setFocused(bool focused)
    {
        if (focused_ == focused)
            return;
        focused_ = focused;
        static_cast<void>(emit(focusChanged, PaneFocusChanged{focused_}));
    }

    void Pane::rebuildWindowLabel()
    {
        window_label_ = title_;
        window_label_ += "###";
        window_label_ += id_.name();
    }
} // namespace lux::ui
