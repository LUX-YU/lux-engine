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
#include <optional>

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
        [[nodiscard]] window::LuxWindow* window() const noexcept;
        [[nodiscard]] const Theme& theme() const noexcept;
        [[nodiscard]] float scale() const noexcept;
        [[nodiscard]] lux::cxx::expected<FontAtlas, EInitError> fontAtlas() const noexcept;
        // Optional capture, immediate resource pinning, remaining input, then owner maintenance.
        // A null output maintains owners without generating another frame or replaying input.
        using Capture = cxx::function_ref<cxx::expected<void, ECaptureError>(const DrawData&)>;
        [[nodiscard]] cxx::expected<void, ECaptureError> update(
            FrameInfo,
            DrawData*,
            std::optional<Capture> = std::nullopt
        ) noexcept;

        [[nodiscard]] PaneResult<std::reference_wrapper<Pane>> addPane(std::unique_ptr<Pane>&&) noexcept;
        [[nodiscard]] PaneResult<void> addPanes(std::span<std::unique_ptr<Pane>>) noexcept;
        [[nodiscard]] PaneResult<std::unique_ptr<Pane>> removePane(Pane&) noexcept;
        [[nodiscard]] PaneResult<void> clearPanes() noexcept;
        [[nodiscard]] PaneResult<void> checkStructureSafe() const noexcept;
        object::TSignal<PaneChanged> paneChanged{*this};

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
        [[nodiscard]] std::span<const std::unique_ptr<Pane>> panes() const noexcept;
        [[nodiscard]] std::uint64_t windowRevision() const noexcept;
        [[nodiscard]] bool menuTargets(const Element&) const noexcept;
        // DIRECT notification; receivers may only invalidate borrows, never destroy other UI objects.
        object::TSignal<object::LuxObject*> objectRemoved{*this};

        // Synchronous borrow only. Stored targets must also retain and validate this Root's ObjectId.
        [[nodiscard]] Pane* findPane(PaneId) const noexcept;
        [[nodiscard]] PaneResult<void> withPane(object::ObjectId root, PaneId, cxx::function_ref<void(Pane&)>) noexcept;
        void showPanes() noexcept;
        [[nodiscard]] Pane* focusedPane() const noexcept;
        [[nodiscard]] bool requestFocus(Pane&) noexcept;
        [[nodiscard]] bool requestFocus(PaneId) noexcept;
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
        [[nodiscard]] cxx::expected<void, EDockError> commitDockTree(PreparedDockTree&&) noexcept;
        [[nodiscard]] DockTree captureDockTree() const;

        void setDockLayout(DockLayout);
        void resetDockLayout(DockLayout = {});
        void clearDockLayout() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EDockError> validateDockLayout(const DockLayout&) const noexcept;

    private:
        Root() noexcept;
        [[nodiscard]] cxx::expected<void, EInitError> initialize(RootConfig) noexcept;
        friend class Pane;
        friend class Element;
        [[nodiscard]] static PaneResult<void> compose(
            Pane*,
            Element*,
            Element& child,
            Element* previous,
            bool replace
        ) noexcept;
        void paneLabelChanged() noexcept;
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
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ui
