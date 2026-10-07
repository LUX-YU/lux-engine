#pragma once

#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/ui/Docking.hpp>
#include <lux/engine/ui/DrawData.hpp>
#include <lux/engine/ui/FontAtlas.hpp>
#include <lux/engine/ui/FontSource.hpp>
#include <lux/engine/ui/Geometry.hpp>
#include <lux/engine/ui/Ids.hpp>
#include <lux/engine/ui/InputEvent.hpp>
#include <lux/engine/ui/Menu.hpp>
#include <lux/engine/ui/PaneError.hpp>
#include <lux/engine/ui/Theme.hpp>
#include <lux/engine/ui/UpdateStatistics.hpp>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace lux::window
{
    class LuxWindow;
}

namespace lux::ui
{
    // A synchronous borrow, intentionally not copyable or queueable.
    struct ObjectRemoved final
    {
        explicit ObjectRemoved(object::LuxObject& value) noexcept : object(&value) {}
        ObjectRemoved(const ObjectRemoved&) = delete;
        ObjectRemoved& operator=(const ObjectRemoved&) = delete;
        object::LuxObject* object;
    };

    class Pane;
    class Element;
    struct SizeHint;

    struct RootConfig final
    {
        Theme theme{Theme::luxDark()};
        bool docking{true};
        const FontSource* font{};
        std::size_t input_capacity{4096};
        std::size_t pane_capacity{65536};
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
    class LUX_FUNCTION_PUBLIC Root final : public lux::object::LuxObject
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<Root>, EInitError>;
        [[nodiscard]] static CreateResult create(RootConfig = {}) noexcept;
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
        [[nodiscard]] const Theme& theme() const noexcept;
        [[nodiscard]] UpdateStatistics statistics() const noexcept;
        [[nodiscard]] float scale() const noexcept;
        [[nodiscard]] lux::cxx::expected<FontAtlas, EInitError> fontAtlas() const noexcept;
        using Capture = cxx::function_ref<cxx::expected<void, ECaptureError>(const DrawData&) noexcept>;
        // Both overloads adopt the prior structural batch first. No drawing or input replay in maintenance-only use.
        [[nodiscard]] cxx::expected<void, ECaptureError> update() noexcept;
        // Capture pins resources synchronously before remaining input and owner maintenance.
        [[nodiscard]] cxx::expected<void, ECaptureError> update(
            FrameInfo,
            DrawData&,
            std::optional<Capture> = std::nullopt
        ) noexcept;

        [[nodiscard]] PaneResult<std::reference_wrapper<Pane>> addPane(std::unique_ptr<Pane>&&) noexcept;
        [[nodiscard]] PaneResult<void> addPanes(std::span<std::unique_ptr<Pane>>) noexcept;
        [[nodiscard]] PaneResult<std::unique_ptr<Pane>> removePane(Pane&) noexcept;
        [[nodiscard]] PaneResult<void> clearPanes() noexcept;
        using PaneCommit = cxx::function_ref<void(std::span<const PaneHandle>) noexcept>;
        // Commit complete structure and semantic ownership before any removal/attachment notification.
        [[nodiscard]] PaneResult<std::vector<std::unique_ptr<Pane>>> replacePanes(
            std::span<const PaneHandle> remove,
            std::span<std::unique_ptr<Pane>> add,
            PaneCommit on_commit
        ) noexcept;
        [[nodiscard]] PaneHandle paneHandle(const Pane&) const noexcept;
        // Owner-thread synchronous borrow only; never retain the pointer across callbacks or frames.
        [[nodiscard]] Pane* resolvePane(PaneHandle) const noexcept;
        object::TSignal<PaneChanged> paneChanged{*this};

        using ChangeCallback = void (*)(object::LuxObject&) noexcept;
        // Non-owning, coalesced target/callback intents; inputs stay in the target owner.
        // Destruction cancels them synchronously. New intents during apply wait for the next batch.
        void deferChange(Pane& target, ChangeCallback apply) noexcept;
        void deferChange(Element& target, ChangeCallback apply) noexcept;

        // Non-owning endpoint for unhandled commands, including commands with no focused Pane.
        void setCommandFallback(object::LuxObject*) noexcept;
        void setMenu(std::vector<MenuItem>);
        [[nodiscard]] std::span<const MenuItem> menu() const noexcept;
        // Borrowed enumeration freezes structure until every callback has returned.
        [[nodiscard]] PaneResult<void> forEachPane(cxx::function_ref<void(Pane&) noexcept>) noexcept;
        // DIRECT notification; receivers may only invalidate borrows, never destroy other UI objects.
        object::TSignal<ObjectRemoved> objectRemoved{*this};

        [[nodiscard]] Pane* focusedPane() const noexcept;
        [[nodiscard]] bool requestFocus(Pane&) noexcept;
        [[nodiscard]] bool capturePointer(Pane&) noexcept;
        void releasePointer(Pane&) noexcept;
        [[nodiscard]] Element* focusedElement() const noexcept;
        [[nodiscard]] bool requestFocus(Element&) noexcept;
        void releaseFocus(Element&) noexcept;
        [[nodiscard]] bool capturePointer(Element&) noexcept;
        void releasePointer(Element&) noexcept;

        // Validate and prepare the entire tree before publishing one pending placement.
        [[nodiscard]] cxx::expected<void, EDockError> setDockTree(DockTree) noexcept;
        [[nodiscard]] DockTree captureDockTree() const;

    private:
        using LuxObject::addChild;
        using LuxObject::removeChild;
        using LuxObject::setParent;
        [[nodiscard]] PaneResult<void> checkStructureSafe() const noexcept;
        struct Impl;
        explicit Root(std::unique_ptr<Impl>) noexcept;
        void applyPendingChanges() noexcept;
        [[nodiscard]] cxx::expected<void, ECaptureError>
        updateFrame(FrameInfo, DrawData*, std::optional<Capture>) noexcept;
        [[nodiscard]] Pane* findPane(PaneId) const noexcept;
        [[nodiscard]] bool menuTargets(const Element&) const noexcept;
        friend class Pane;
        friend class Element;
        [[nodiscard]] static PaneResult<void> compose(
            Pane*,
            Element*,
            Element& child,
            Element* previous,
            bool replace
        ) noexcept;
        void releasePane(Pane&) noexcept;
        void releaseElement(Element&) noexcept;
        void notifyRemoved(object::LuxObject&) noexcept;
        [[nodiscard]] bool attachmentSafe() const noexcept;
        bool allowsGenericStructure() const noexcept override
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
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ui
