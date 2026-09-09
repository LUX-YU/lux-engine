#pragma once
// Concrete Scene UI borrows its Session explicitly. Generic EditorWindow is independent of Scene business.
#include <lux/engine/editor/ui/shell/EditorWindow.hpp>
#include <lux/engine/editor/ui/actions/HistoryActions.hpp>
#include <lux/engine/editor/sessions/scene/SceneView.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/ViewportElement.hpp>
#include <memory>
#include <lux/engine/editor/ui/scene/visibility.h>
#include <lux/engine/ui/UiInputEvent.hpp>
#include <span>
namespace lux::editor::ui
{
    // Scene-specific boundary: preserve owning source failures without adding Scene to generic editor_ui.
    struct SceneWorkspaceFailure final
    {
        std::optional<WindowFailure> window;
        std::optional<sessions::SceneFailure> scene;
        SceneWorkspaceFailure(WindowFailure failure) noexcept : window(std::move(failure)) {}
        SceneWorkspaceFailure(sessions::SceneFailure failure) noexcept : scene(std::move(failure)) {}
    };
    template <class T> using SceneWorkspaceResult = lux::cxx::expected<T, SceneWorkspaceFailure>;
    class LUX_EDITOR_SCENE_UI_PUBLIC SceneViewport final : public lux::object::Object<SceneViewport, lux::ui::Pane>
    {
    public:
        SceneViewport(lux::object::ObjectDispatcherRef, lux::ui::PaneId, sessions::SceneView &);
        ~SceneViewport() noexcept override;
        // Owner-thread borrows only. Foreign-thread snapshot returns empty; void helpers reject by doing no work.
        [[nodiscard]] std::span<const rendering::ViewImage> frameImages() const noexcept;
        void cancelCapture() noexcept;
        void consumeInput(const lux::ui::UiInputSnapshot &, double, lux::ui::Vec2) noexcept;
        [[nodiscard]] std::optional<sessions::SceneFailure> actionFailure() const noexcept;
        void releaseFrameImages() noexcept;

    private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        struct Impl; // Only viewport input/capture/last-frame display refs, no scene jobs or renderer pump.
        std::unique_ptr<Impl> impl_;
    };
    class LUX_EDITOR_SCENE_UI_PUBLIC SceneOutliner final : public lux::object::Object<SceneOutliner, lux::ui::Pane>
    {
    public:
        SceneOutliner(lux::object::ObjectDispatcherRef, lux::ui::PaneId, sessions::SceneSession &);
        ~SceneOutliner() noexcept override;

    private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        struct Impl; // Filter/expansion/row-view cache. Selection authority is in Session.
        std::unique_ptr<Impl> impl_;
    };
    class LUX_EDITOR_SCENE_UI_PUBLIC SceneInspector final : public lux::object::Object<SceneInspector, lux::ui::Pane>
    {
    public:
        SceneInspector(lux::object::ObjectDispatcherRef, lux::ui::PaneId, sessions::SceneSession &);
        ~SceneInspector() noexcept override;
        [[nodiscard]] WindowResult<void> installReaders(std::span<const struct ComponentReadBinding>) noexcept;

    private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        struct Impl; // Local property input state + bound reader descriptors, never a content undo stack.
        std::unique_ptr<Impl> impl_;
    };
    class LUX_EDITOR_SCENE_UI_PUBLIC SceneResourcesPane final
        : public lux::object::Object<SceneResourcesPane, lux::ui::Pane>
    {
    public:
        SceneResourcesPane(lux::object::ObjectDispatcherRef, lux::ui::PaneId, sessions::SceneSession &);
        ~SceneResourcesPane() noexcept override;

    private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        struct Impl; // Status display/filter; retry is explicit Session operation.
        std::unique_ptr<Impl> impl_;
    };
    class LUX_EDITOR_SCENE_UI_PUBLIC SceneWorkspace final : public lux::object::Object<SceneWorkspace>
    {
    public:
        [[nodiscard]] static WindowResult<std::unique_ptr<SceneWorkspace>> create(
            EditorWindow &, WorkspaceId, sessions::SceneSession &, std::unique_ptr<sessions::SceneView> &) noexcept;
        // Create failure retains caller's view. Session outlives workspace; closing workspace never closes Session.
        ~SceneWorkspace() noexcept override;
        SceneWorkspace(const SceneWorkspace &) = delete;
        SceneWorkspace &operator=(const SceneWorkspace &) = delete;
        SceneWorkspace(SceneWorkspace &&) = delete;
        SceneWorkspace &operator=(SceneWorkspace &&) = delete;
        [[nodiscard]] WorkspaceId id() const noexcept;
        [[nodiscard]] SceneWorkspaceResult<void> updateBeforeFrame() noexcept;
        [[nodiscard]] WindowResult<void> afterDraw(double, lux::ui::Vec2) noexcept;
        // Foreign threads cannot borrow or release the Pane's current frame images.
        void releaseFrameImages() noexcept;
        [[nodiscard]] WindowResult<void> activate() noexcept;
        [[nodiscard]] std::span<const rendering::ViewImage> frameImages() const noexcept;
        [[nodiscard]] WindowResult<void> beginClose() noexcept;
        [[nodiscard]] SceneWorkspaceResult<sessions::ECloseProgress> advanceClose() noexcept;

    private:
        struct Impl; // Owns real panes/view/registrations/connections/layout; no editable source model.
        SceneWorkspace(lux::object::ObjectDispatcherRef, std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::ui
