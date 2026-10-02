#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/detail/Contract.hpp>
#include <lux/engine/ui/detail/AttachmentState.hpp>

#include <algorithm>
#include <utility>

namespace lux::ui
{
    Pane::Pane(object::ObjectDispatcherRef dispatcher, PaneId id, PaneTypeId type, std::string title)
        : LuxObject(std::move(dispatcher)), id_(std::move(id)), type_(std::move(type)), title_(std::move(title))
    {
        if (!id_.isValid())
            detail::failContract();
        rebuildWindowLabel();
    }
    void Pane::invalidatePreparation() noexcept
    {
        for (auto* node = this; node; node = dynamic_cast<Pane*>(node->parent()))
            if (node->preparation_)
                node->preparation_->valid = false;
    }

    Pane::Pane(Pane& parent, PaneId id, PaneTypeId type, std::string title)
        : Pane(parent.dispatcherRef(), std::move(id), std::move(type), std::move(title))
    {
        root_ = parent.attachedRoot();
        if (root_)
            root_->checkContentChange();
        parent.invalidatePreparation();
        attachTo(parent);
        if (root_)
            root_->registerPane(*this);
    }

    Pane::~Pane()
    {
        invalidatePreparation();
        if (preparation_)
        {
            for (auto*& pane : preparation_->roots)
                if (pane == this)
                    pane = nullptr;
            preparation_ = nullptr;
        }
        if (root_)
            root_->unregisterPane(*this);
    }

    Root& Pane::root() const noexcept
    {
        if (!root_)
            detail::failContract();
        return *root_;
    }

    void Pane::setContent(Element& element) noexcept
    {
        invalidatePreparation();
        if (root_)
            root_->checkContentChange();
        if (!element.parent() && !root_)
        {
            element.attachContent(*this);
        }
        if (!isOnAffinityThread() || element.parent() != this)
            detail::failContract();
        content_ = &element;
    }

    void Pane::setTitle(std::string title)
    {
        if (title_ == title)
            return;
        invalidatePreparation();
        title_ = std::move(title);
        rebuildWindowLabel();
        if (root_)
            root_->paneLabelChanged();
    }

    void Pane::setVisible(bool visible)
    {
        if (visible_ == visible)
            return;
        invalidatePreparation();
        visible_ = visible;
        if (root_)
            root_->paneLabelChanged();
        static_cast<void>(emit(visibilityChanged, PaneVisibilityChanged{visible_}));
    }

    void Pane::setModal(bool modal) noexcept
    {
        invalidatePreparation();
        if (root_)
            root_->checkContentChange();
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
