#include <algorithm>
#include <cmath>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/detail/Contract.hpp>

namespace lux::ui
{
    namespace
    {
        bool validSize(Size size, bool infinite = false) noexcept
        {
            const bool valid_width =
                size.width >= 0 && (infinite ? !std::isnan(size.width) : std::isfinite(size.width));
            const bool valid_height =
                size.height >= 0 && (infinite ? !std::isnan(size.height) : std::isfinite(size.height));
            return valid_width && valid_height;
        }
    } // namespace

    Root* Element::attachedRoot() const noexcept
    {
        return pane_ ? pane_->attachedRoot() : nullptr;
    }
    void Element::assignPane(Pane* pane) noexcept
    {
        auto* node = this;
        for (;;)
        {
            node->pane_ = pane;
            if (node->firstChild())
            {
                node = static_cast<Element*>(node->firstChild());
            }
            else
            {
                while (node != this && !node->nextSibling())
                {
                    node = static_cast<Element*>(node->parent());
                }
                if (node == this)
                {
                    return;
                }
                node = static_cast<Element*>(node->nextSibling());
            }
        }
    }
    PaneResult<void> Element::addElement(Element& child) noexcept
    {
        if (!acceptsElements())
        {
            return cxx::unexpected(EPaneError::INVALID_TREE);
        }
        return Root::compose(nullptr, this, child, nullptr, false);
    }

    PaneResult<void> Element::replaceElement(Element& previous, Element& child) noexcept
    {
        if (!acceptsElements())
        {
            return cxx::unexpected(EPaneError::INVALID_TREE);
        }
        return Root::compose(nullptr, this, child, &previous, true);
    }

    Element::~Element() noexcept
    {
        auto* root = attachedRoot();
        if (root)
        {
            root->checkContentChange();
            root->checkDestruction(*this);
        }
        beginDestruction();
        if (root)
        {
            root->releaseElement(*this);
        }
        if (pane_ && pane_->content_ == this)
        {
            pane_->content_ = nullptr;
        }
        commitRelation(*this, nullptr);
        if (root)
        {
            root->notifyRemoved(*this);
        }
        clearElements();
    }

    void Element::clearElements() noexcept
    {
        auto* root = attachedRoot();
        if (root)
        {
            root->checkContentChange();
        }
        while (auto* child = static_cast<Element*>(firstChild()))
        {
            if (root)
            {
                root->releaseElement(*child);
            }
            commitRelation(*child, nullptr);
            child->assignPane(nullptr);
            child->element_parent_ = nullptr;
            if (root)
            {
                root->notifyRemoved(*child);
            }
        }
    }
    Root& Element::root() const noexcept
    {
        return pane().root();
    }
    Pane& Element::pane() const noexcept
    {
        if (!pane_)
        {
            detail::failContract();
        }
        return *pane_;
    }
    bool Element::focused() const noexcept
    {
        const auto* attached = attachedRoot();
        return attached && attached->focusedElement() == this;
    }
    bool Element::displayed() const noexcept
    {
        if (!attachedRoot() || !pane_->visible())
        {
            return false;
        }
        for (auto* node = this; node; node = node->element_parent_)
        {
            if (!node->visible())
            {
                return false;
            }
        }
        return true;
    }
    void Element::setVisible(bool visible) noexcept
    {
        if (!isOnAffinityThread())
        {
            detail::failContract();
        }
        visible_ = visible;
    }
    void Element::setEnabled(bool enabled) noexcept
    {
        if (!isOnAffinityThread())
        {
            detail::failContract();
        }
        enabled_ = enabled;
    }
    void Element::setMinimumSize(Size size) noexcept
    {
        if (!isOnAffinityThread() || !validSize(size))
        {
            detail::failContract();
        }
        minimum_ = size;
    }
    void Element::setMaximumSize(Size size) noexcept
    {
        if (!isOnAffinityThread() || !validSize(size, true))
        {
            detail::failContract();
        }
        maximum_ = size;
    }
    void Element::setStretch(Vec2 weight) noexcept
    {
        if (!isOnAffinityThread() || !validSize({weight.x, weight.y}))
        {
            detail::failContract();
        }
        stretch_ = weight;
    }
    void Element::setAlignment(EAlignment horizontal, EAlignment vertical) noexcept
    {
        if (!isOnAffinityThread())
        {
            detail::failContract();
        }
        horizontal_alignment_ = horizontal;
        vertical_alignment_ = vertical;
    }
    SizeHint Element::sizeHintContent() noexcept
    {
        return {};
    }
    SizeHint Element::measureContent(float) noexcept
    {
        return sizeHint();
    }
    SizeHint Element::sizeHint() noexcept
    {
        return root().measureElement(*this, 0.F, true);
    }
    SizeHint Element::measure(float width) noexcept
    {
        if (!isOnAffinityThread() || !std::isfinite(width) || width < 0.F)
        {
            detail::failContract();
        }
        return root().measureElement(*this, width, false);
    }
    SizeHint Element::constrain(SizeHint hint) const noexcept
    {
        const bool invalid_hint =
            !validSize(hint.minimum) || !validSize(hint.preferred) || !validSize(hint.maximum, true);
        if (invalid_hint)
        {
            detail::failContract();
        }
        hint.minimum.width = std::max(hint.minimum.width, minimum_.width);
        hint.minimum.height = std::max(hint.minimum.height, minimum_.height);
        hint.maximum.width = std::max(hint.minimum.width, std::min(hint.maximum.width, maximum_.width));
        hint.maximum.height = std::max(hint.minimum.height, std::min(hint.maximum.height, maximum_.height));
        hint.preferred.width = std::clamp(hint.preferred.width, hint.minimum.width, hint.maximum.width);
        hint.preferred.height = std::clamp(hint.preferred.height, hint.minimum.height, hint.maximum.height);
        return hint;
    }
    void Element::arrange(Rect rect) noexcept
    {
        const bool valid_origin = std::isfinite(rect.position.x) && std::isfinite(rect.position.y);
        if (!isOnAffinityThread() || !valid_origin || !validSize(rect.size))
        {
            detail::failContract();
        }
        rect_ = rect;
        root().arrangeElement(*this);
    }
    void Element::drawChild(Element& child, Point offset) noexcept
    {
        if (child.parent() != this)
        {
            detail::failContract();
        }
        root().drawElement(child, {draw_origin_.x + offset.x, draw_origin_.y + offset.y});
    }
} // namespace lux::ui
