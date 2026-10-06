#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ui/Ids.hpp>
#include <lux/engine/ui/PaneError.hpp>

namespace lux::ui
{
    namespace detail { struct RootTestAccess; }
    class Root;
    class Element;

    struct PaneFocusChanged final
    {
        bool focused{false};
    };

    struct PaneVisibilityChanged final
    {
        bool visible{false};
    };

    class LUX_FUNCTION_PUBLIC Pane : public lux::object::LuxObject
    {
    public:
        /**
     * Observers may update owner state, but must not synchronously destroy
     * this Pane from either callback. Defer destruction until the current
     * UI/Signal stack has returned to its owner-thread safe point.
     */
        object::TSignal<PaneFocusChanged> focusChanged{*this};
        object::TSignal<PaneVisibilityChanged> visibilityChanged{*this};
        object::TSignal<> closeRequested{*this};

        explicit Pane(std::string title);
        [[nodiscard]] Root* attachedRoot() const noexcept
        {
            return root_;
        }

        ~Pane() override;

        [[nodiscard]] std::string_view title() const noexcept
        {
            return title_;
        }
        [[nodiscard]] bool visible() const noexcept
        {
            return visible_;
        }
        [[nodiscard]] bool focused() const noexcept
        {
            return focused_;
        }
        [[nodiscard]] bool hovered() const noexcept
        {
            return hovered_;
        }

        // The owner decides whether to close, hide or cancel. Notifications do not consume the intent.
        void requestClose() noexcept;
        [[nodiscard]] bool hasCloseRequest() const noexcept
        {
            return close_requested_;
        }
        void dismissCloseRequest() noexcept;
        void setTitle(std::string title);
        void setVisible(bool visible);
        // A modal is still one window, but cannot join the shared dock space.
        // Set during owner maintenance, before the next draw.
        void setModal(bool modal) noexcept;
        [[nodiscard]] bool modal() const noexcept
        {
            return modal_;
        }
        [[nodiscard]] Root& root() const noexcept;
        [[nodiscard]] PaneResult<void> addElement(Element&) noexcept;
        [[nodiscard]] PaneResult<void> replaceContent(Element&) noexcept;
        [[nodiscard]] Element* content() const noexcept
        {
            return content_;
        }

    protected:
        void clearContent() noexcept;
        virtual void update() noexcept {}

    private:
        friend struct detail::RootTestAccess;
        friend class Root;
        friend class Element;
        void setFocused(bool focused);
        void setHovered(bool hovered) noexcept
        {
            hovered_ = hovered;
        }
        void rebuildWindowLabel();
        bool allowsGenericStructure() const noexcept final { return false; }
        PaneId id_;
        std::string title_;
        std::string window_label_;
        bool visible_{true};
        bool focused_{false};
        bool hovered_{false};
        bool modal_{};
        bool close_requested_{};
        Element* content_{};
        Root* root_{};
    };
} // namespace lux::ui
