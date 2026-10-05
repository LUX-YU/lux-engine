#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <lux/engine/function/visibility.h>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ui/Ids.hpp>
#include <lux/engine/ui/Attachment.hpp>

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
        [[nodiscard]] cxx::expected<void, EAttachmentError> addSubPane(Pane&) noexcept;
        template <class T, class D>
            requires std::derived_from<T, Pane> && std::same_as<typename std::unique_ptr<T, D>::pointer, T*> &&
                     (!std::is_reference_v<D>) && std::is_nothrow_move_constructible_v<D> &&
                     std::is_nothrow_destructible_v<D>
        [[nodiscard]] cxx::expected<void, EAttachmentError>
        addSubPane(std::unique_ptr<T, D>&& candidate) noexcept
        {
            if (!candidate)
                return cxx::unexpected(EAttachmentError::INVALID_TREE);
            auto attach = [&]() noexcept -> object::ObjectResult<void>
            {
                auto adopted = adoptChild(std::move(candidate));
                if (!adopted)
                    return cxx::unexpected(adopted.error());
                return {};
            };
            return addSubPaneImpl(*candidate, attach);
        }
        [[nodiscard]] cxx::expected<void, EAttachmentError> setContent(Element&) noexcept;
        template <class T, class D>
            requires std::derived_from<T, Element> && std::same_as<typename std::unique_ptr<T, D>::pointer, T*> &&
                     (!std::is_reference_v<D>) && std::is_nothrow_move_constructible_v<D> &&
                     std::is_nothrow_destructible_v<D>
        [[nodiscard]] cxx::expected<void, EAttachmentError>
        setContent(std::unique_ptr<T, D>&& candidate) noexcept
        {
            if (!candidate)
                return cxx::unexpected(EAttachmentError::INVALID_TREE);
            auto attach = [&]() noexcept -> object::ObjectResult<void>
            {
                auto adopted = adoptChild(std::move(candidate));
                if (!adopted)
                    return cxx::unexpected(adopted.error());
                return {};
            };
            return setContentImpl(*candidate, false, attach);
        }
        // The owner ends domain editing before preparing a replacement; this operation only changes UI structure.
        [[nodiscard]] cxx::expected<void, EAttachmentError> replaceContent(Element&) noexcept;
        template <class T, class D>
            requires std::derived_from<T, Element> && std::same_as<typename std::unique_ptr<T, D>::pointer, T*> &&
                     (!std::is_reference_v<D>) && std::is_nothrow_move_constructible_v<D> &&
                     std::is_nothrow_destructible_v<D>
        [[nodiscard]] cxx::expected<void, EAttachmentError>
        replaceContent(std::unique_ptr<T, D>&& candidate) noexcept
        {
            if (!candidate)
                return cxx::unexpected(EAttachmentError::INVALID_TREE);
            auto attach = [&]() noexcept -> object::ObjectResult<void>
            {
                auto adopted = adoptChild(std::move(candidate));
                if (!adopted)
                    return cxx::unexpected(adopted.error());
                return {};
            };
            return setContentImpl(*candidate, true, attach);
        }
        [[nodiscard]] Element* content() const noexcept
        {
            return content_;
        }

    protected:
        // Use before members die when parent-owned content borrows those members.
        void clearChildren() noexcept;
        virtual void update() noexcept {}

    private:
        friend class PreparedAttachment;
        friend class Root;
        friend class Element;
        [[nodiscard]] cxx::expected<void, EAttachmentError> addSubPaneImpl(
            Pane&, cxx::function_ref<object::ObjectResult<void>()>
        ) noexcept;
        [[nodiscard]] cxx::expected<void, EAttachmentError> setContentImpl(
            Element&, bool replace, cxx::function_ref<object::ObjectResult<void>()>
        ) noexcept;
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
        std::uint64_t attachment_epoch_{};

        PaneId id_;
        PaneTypeId type_;
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
