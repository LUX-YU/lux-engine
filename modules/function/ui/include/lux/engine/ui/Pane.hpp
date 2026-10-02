#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ui/Ids.hpp>

namespace lux::ui
{
    class Root;
    class Element;
    namespace detail
    {
        struct AttachmentState;
    }

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

        Pane(object::ObjectDispatcherRef, PaneId id, PaneTypeId type, std::string title);
        [[nodiscard]] Root* attachedRoot() const noexcept
        {
            return root_;
        }
        Pane(Pane& parent, PaneId id, PaneTypeId type, std::string title);

        ~Pane() override;

        [[nodiscard]] const PaneId& id() const noexcept
        {
            return id_;
        }
        [[nodiscard]] const PaneTypeId& type() const noexcept
        {
            return type_;
        }
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

        void requestClose() noexcept
        {
            static_cast<void>(emit(closeRequested));
        }
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
        void setContent(Element&) noexcept;
        [[nodiscard]] Element* content() const noexcept
        {
            return content_;
        }

    protected:
        virtual void update() noexcept {}

    private:
        friend class PreparedAttachment;
        friend class Root;
        friend class Element;
        void setFocused(bool focused);
        void setHovered(bool hovered) noexcept
        {
            hovered_ = hovered;
        }
        void rebuildWindowLabel();
        bool allowsGenericChildren() const noexcept override
        {
            return false;
        }
        void invalidatePreparation() noexcept;
        detail::AttachmentState* preparation_{};
        std::size_t registration_slot_{SIZE_MAX}, window_slot_{SIZE_MAX};

        PaneId id_;
        PaneTypeId type_;
        std::string title_;
        std::string window_label_;
        bool visible_{true};
        bool focused_{false};
        bool hovered_{false};
        bool modal_{};
        Element* content_{};
        Root* root_{};
    };
} // namespace lux::ui
