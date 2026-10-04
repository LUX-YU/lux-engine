#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ui/Geometry.hpp>
#include <lux/engine/ui/Docking.hpp>
#include <lux/engine/ui/Theme.hpp>
#include <lux/engine/ui/FontSource.hpp>
#include <lux/engine/ui/FontAtlas.hpp>
#include <lux/engine/ui/DrawData.hpp>
#include <lux/engine/ui/InputEvent.hpp>
#include <lux/engine/ui/Ids.hpp>
#include <lux/engine/ui/Menu.hpp>
#include <lux/engine/ui/Attachment.hpp>
#include <lux/cxx/core/function_ref.hpp>

namespace lux::window
{
    class LuxWindow;
}

namespace lux::ui
{
    class Pane;
    class Element;
    struct SizeHint;

    struct RootConfig final
    {
        Theme theme{Theme::luxDark()};
        bool docking{true};
        const FontSource* font{};
        std::size_t input_capacity{4096};
        std::size_t attachment_capacity{65536};
        // Explicit content-unit scale. Independent from FrameInfo::framebuffer_scale.
        float scale{1.f};
    };

    struct FrameInfo final
    {
        Size display_size;
        float delta_seconds{};
        Vec2 framebuffer_scale{1.0F, 1.0F};
    };

    // CPU UI ownership. No window, Scene, device or render thread is owned here.
    class LUX_FUNCTION_PUBLIC Root : public lux::object::LuxObject
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<Root>, EInitError>;
        [[nodiscard]] static CreateResult create(object::ObjectDispatcherRef, RootConfig = {}) noexcept;
        ~Root() noexcept override;

        // FULL leaves this event unaccepted, including both physical/aggregate modifiers.
        // Native sequences are monotonic per Root/window. Zero assigns a local
        // sequence (synthetic input/tests). FULL does not consume the sequence.
        [[nodiscard]] lux::cxx::expected<void, EInputError> feedInput(
            const VInputEvent&,
            std::uint64_t sequence = 0
        ) noexcept;
        void closeInput() noexcept;
        [[nodiscard]] InputSnapshot inputSnapshot() const noexcept;
        // Optional platform attachment. CPU-only roots have no native window.
        void bindWindow(window::LuxWindow*) noexcept;
        [[nodiscard]] window::LuxWindow* window() const noexcept;
        [[nodiscard]] const Theme& theme() const noexcept;
        [[nodiscard]] float scale() const noexcept;
        [[nodiscard]] lux::cxx::expected<FontAtlas, EInitError> fontAtlas() const noexcept;
        // Optional capture, immediate resource pinning, remaining input, then owner maintenance.
        // A null output maintains owners without generating another frame or replaying input.
        [[nodiscard]] lux::cxx::expected<void, ECaptureError> update(FrameInfo, DrawData* output) noexcept;

        [[nodiscard]] cxx::expected<void, EAttachmentError> addSubPane(Pane&) noexcept;
        [[nodiscard]] cxx::expected<void, EAttachmentError> removeSubPane(Pane&) noexcept;
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

        // Complete owned window batch. A refusal leaves every owner/deleter and the current UI intact.
        // Docking is already validated by prepareDockTree and is consumed only after all windows commit.
        [[nodiscard]] cxx::expected<AttachmentCommit, EAttachmentError> addSubPanes(
            std::span<std::unique_ptr<Pane, object::ObjectDeleter>>,
            std::span<const WindowVisibility> = {},
            PreparedDockTree* docking = nullptr
        ) noexcept;

        // Cold-path preparation reserves the complete subtree. Commit is owner-thread and outside callbacks.
        using AttachmentResult = lux::cxx::expected<PreparedAttachment, EAttachmentError>;
        [[nodiscard]] AttachmentResult prepareMount(Pane&);
        [[nodiscard]] AttachmentResult prepareDetach(Pane&);
        [[nodiscard]] AttachmentResult prepareMount(std::span<Pane* const>, std::span<const WindowVisibility> = {});
        [[nodiscard]] AttachmentResult prepareDetach(std::span<Pane* const>);
        [[nodiscard]] lux::cxx::expected<AttachmentCommit, EAttachmentError> commit(PreparedAttachment&) noexcept;
        // The host transfers already-prepared local state after all links change, before any notification.
        // This synchronous callback must not allocate, dispatch, call providers or mutate the object tree.
        [[nodiscard]] lux::cxx::expected<AttachmentCommit, EAttachmentError> commit(
            PreparedAttachment&,
            cxx::function_ref<void()> adopt
        ) noexcept;
        object::TSignal<AttachmentChanged> attachmentChanged{*this};

        using ChangeCallback = void (*)(object::LuxObject&) noexcept;
        // Non-owning, coalesced target/callback intents; inputs stay in the target owner.
        // Destruction cancels them synchronously. New intents during apply wait for the next batch.
        void deferChange(Pane& target, ChangeCallback apply) noexcept;
        void deferChange(Element& target, ChangeCallback apply) noexcept;
        // Host-only boundary, outside drawing, measurement, update and object dispatch.
        // Neither update() nor a resource wait drains this queue implicitly.
        void applyPendingChanges() noexcept;
        [[nodiscard]] bool hasPendingChanges() const noexcept;

        // One source keeps all borrowed IDs/text alive. Empty owner is only for static menu data.
        void setMenu(std::vector<MenuItem>, std::shared_ptr<const void> source);
        [[nodiscard]] std::span<const MenuItem> menu() const noexcept;
        [[nodiscard]] std::span<Pane* const> panes() const noexcept;
        [[nodiscard]] std::uint64_t windowRevision() const noexcept;
        [[nodiscard]] bool menuTargets(const Element&) const noexcept;
        // DIRECT notification; receivers may only invalidate borrows, never destroy other UI objects.
        object::TSignal<object::LuxObject*> objectRemoved{*this};

        [[nodiscard]] Pane* findPane(PaneIdView) const noexcept;
        [[nodiscard]] cxx::expected<PaneHandle, EAttachmentError> identify(const Pane&) const noexcept;
        // A borrowed pointer valid only until the next callback or structure change.
        [[nodiscard]] cxx::expected<Pane*, EAttachmentError> findPane(const PaneHandle&) const noexcept;
        // Synchronous owner-stage access. The original Object borrow protects this Root and window
        // through callbacks/retirement; no pointer escapes and no second window owner is introduced.
        // The callback may update its content, but removal and nested root maintenance are refused.
        [[nodiscard]] cxx::expected<void, EAttachmentError>
        withPane(const PaneHandle&, cxx::function_ref<void(Pane&)>) noexcept;
        void showPanes() noexcept;
        [[nodiscard]] Pane* focusedPane() const noexcept;
        [[nodiscard]] bool requestFocus(Pane&) noexcept;
        [[nodiscard]] bool requestFocus(PaneIdView) noexcept;
        [[nodiscard]] bool capturePointer(Pane&) noexcept;
        void releasePointer(Pane&) noexcept;
        [[nodiscard]] Element* focusedElement() const noexcept;
        [[nodiscard]] bool requestFocus(Element&) noexcept;
        void releaseFocus(Element&) noexcept;
        [[nodiscard]] bool capturePointer(Element&) noexcept;
        void releasePointer(Element&) noexcept;

        // Preparation is side-effect free. Commit only transfers owned values; ImGui adopts them before
        // the next window draw. It does not open a view or resolve a content identity.
        [[nodiscard]] lux::cxx::expected<PreparedDockTree, EDockError> prepareDockTree(DockTree) const;
        void commitDockTree(PreparedDockTree&&) noexcept;
        [[nodiscard]] DockTree captureDockTree() const;

        void setDockLayout(DockLayout);
        void resetDockLayout(DockLayout = {});
        void clearDockLayout() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EDockError> validateDockLayout(const DockLayout&) const noexcept;
        [[nodiscard]] DockState captureDockState() const;
        [[nodiscard]] lux::cxx::expected<void, EDockError> restoreDockState(
            const DockState&,
            std::span<const DockIdentity> identities = {}
        );

    protected:
        explicit Root(object::ObjectDispatcherRef) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EInitError> initialize(RootConfig) noexcept;
        [[nodiscard]] virtual lux::cxx::expected<void, ECaptureError> drawDataReady(const DrawData&) noexcept;

    private:
        friend class PreparedAttachment;
        friend class Pane;
        friend class Element;
        [[nodiscard]] cxx::expected<void, EAttachmentError> addSubPaneImpl(
            Pane&, cxx::function_ref<object::ObjectResult<void>()>
        ) noexcept;
        [[nodiscard]] static cxx::expected<void, EAttachmentError> compose(
            object::LuxObject&, object::LuxObject&, bool replace,
            cxx::function_ref<object::ObjectResult<void>()>, Element* previous = nullptr
        ) noexcept;
        [[nodiscard]] cxx::expected<void, EAttachmentError>
        prepareRegistration(detail::AttachmentState&, std::size_t removing = 0);
        void registerPane(Pane&);
        void registerElement(Element&);
        void unregisterPane(Pane&, bool notify = true) noexcept;
        void paneLabelChanged() noexcept;
        void unregisterElement(Element&, bool notify = true) noexcept;
        void releaseSubtree(object::LuxObject&, bool notify) noexcept;
        static void prepareChildrenRelease(object::LuxObject&) noexcept;
        [[nodiscard]] bool attachmentSafe() const noexcept;
        [[nodiscard]] AttachmentResult prepareAttachment(
            std::span<Pane* const>,
            bool mount,
            std::span<const WindowVisibility> = {}
        );
        void abandonAttachment(detail::AttachmentState&) noexcept;
        bool allowsGenericChildren() const noexcept override
        {
            return false;
        }
        void drawElement(Element&, Point) noexcept;
        [[nodiscard]] SizeHint measureElement(Element&, float width, bool intrinsic) noexcept;
        void arrangeElement(Element&) noexcept;
        void checkDestruction(const object::LuxObject&) const noexcept;
        void checkContentChange() const noexcept;
        void requireOwner() const noexcept;
        [[nodiscard]] lux::cxx::expected<void, ECaptureError> collectDrawData(FrameInfo, DrawData&) noexcept;
        void maintain() noexcept;
        void drawPane(Pane&) noexcept;
        void drawPaneContent(Pane&) noexcept;
        void routeInput() noexcept;
        void deliverWindowFocus(bool focused) noexcept;
        [[nodiscard]] bool allowedByModal(const Pane&) const noexcept;
        [[nodiscard]] bool allowedByModal(const Element&) const noexcept;
        [[nodiscard]] Pane* modalPane() const noexcept;
        void prepareLayout() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ui
