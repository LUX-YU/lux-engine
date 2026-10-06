#include <charconv>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/detail/Contract.hpp>

#include <algorithm>
#include <utility>

namespace lux::ui
{
    Pane::Pane(std::string title) : title_(std::move(title)) {}

    Pane::~Pane()
    {
        if (root_)
        {
            detail::failContract(); // Only Root can surrender a registered window's unique owner.
        }
        beginDestruction();
        clearContent();
    }

    void Pane::clearContent() noexcept
    {
        if (!content_)
        {
            return;
        }
        if (root_)
        {
            root_->checkContentChange();
            root_->releaseElement(*content_);
        }
        auto* previous = std::exchange(content_, nullptr);
        commitRelation(*previous, nullptr);
        previous->assignPane(nullptr);
        previous->element_parent_ = nullptr;
        if (root_)
        {
            root_->notifyRemoved(*previous);
        }
    }

    Root& Pane::root() const noexcept
    {
        if (!root_)
        {
            detail::failContract();
        }
        return *root_;
    }

    PaneResult<void> Pane::addElement(Element& element) noexcept
    {
        return Root::compose(this, nullptr, element, content_, false);
    }

    PaneResult<void> Pane::replaceContent(Element& element) noexcept
    {
        return Root::compose(this, nullptr, element, content_, true);
    }

    void Pane::requestClose() noexcept
    {
        if (!isOnAffinityThread())
        {
            detail::failContract();
        }
        close_requested_ = true;
        static_cast<void>(emit(closeRequested));
    }

    void Pane::dismissCloseRequest() noexcept
    {
        if (!isOnAffinityThread())
        {
            detail::failContract();
        }
        close_requested_ = false;
    }

    void Pane::setTitle(std::string title)
    {
        if (title_ == title)
        {
            return;
        }
        title_ = std::move(title);
        rebuildWindowLabel();
        if (root_)
        {
            root_->paneLabelChanged();
        }
    }

    void Pane::setVisible(bool visible)
    {
        if (visible_ == visible)
        {
            return;
        }
        visible_ = visible;
        if (root_)
        {
            root_->paneLabelChanged();
        }
        static_cast<void>(emit(visibilityChanged, PaneVisibilityChanged{visible_}));
    }

    void Pane::setModal(bool modal) noexcept
    {
        if (root_)
        {
            root_->checkContentChange();
        }
        modal_ = modal;
    }

    void Pane::setFocused(bool focused)
    {
        if (focused_ == focused)
        {
            return;
        }
        focused_ = focused;
        static_cast<void>(emit(focusChanged, PaneFocusChanged{focused_}));
    }

    void Pane::rebuildWindowLabel()
    {
        window_label_ = title_;
        window_label_ += "###";
        char identity[48];
        auto first = std::to_chars(std::begin(identity), std::end(identity), id_.index);
        *first.ptr++ = ':';
        auto second = std::to_chars(first.ptr, std::end(identity), id_.gen);
        window_label_.append(identity, second.ptr);
    }
} // namespace lux::ui
