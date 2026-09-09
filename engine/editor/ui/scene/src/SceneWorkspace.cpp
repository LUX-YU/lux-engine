#include <lux/engine/editor/ui/scene/SceneWorkspace.hpp>
#include <lux/engine/editor/ui/scene/SceneLayout.hpp>
#include <lux/engine/ui/CommandRouter.hpp>
#include <array>
#include <thread>
namespace lux::editor::ui
{
    namespace
    {
        auto fail(EWindowError code) noexcept
        {
            return lux::cxx::unexpected(WindowFailure{code});
        }
        class SceneToolbar final : public lux::object::Object<SceneToolbar, lux::ui::Pane>
        {
        public:
            SceneToolbar(lux::object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, EditorWindow &window)
                : Object(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.scene.toolbar"}, "Workbench"),
                  menu_(std::move(dispatcher), window.activeHistory()), commands_(window.uiSession().commandRouter())
            {
                const auto define = [&](std::string name, std::string label) -> std::optional<lux::ui::CommandHandle>
                {
                    auto existing = commands_.findCommand(lux::ui::UiCommandIdView{name});
                    if (existing)
                        return existing;
                    auto created = commands_.defineCommand({lux::ui::UiCommandId{std::move(name)}, std::move(label)});
                    return created ? std::optional{*created} : std::nullopt;
                };
                auto undo = define(std::string(this->id().name()) + ".menu.undo", "Undo");
                auto redo = define(std::string(this->id().name()) + ".menu.redo", "Redo");
                if (!undo || !redo)
                    return;
                undo_ = *undo;
                redo_ = *redo;
                auto first =
                    commands_.bindGlobal<&HistoryMenuActions::undo, &HistoryMenuActions::canUndo>(undo_, menu_);
                auto second =
                    commands_.bindGlobal<&HistoryMenuActions::redo, &HistoryMenuActions::canRedo>(redo_, menu_);
                if (!first || !second)
                    return;
                undo_binding_ = std::move(*first);
                redo_binding_ = std::move(*second);
                valid_ = true;
            }
            bool valid() const noexcept
            {
                return valid_;
            }
            bool takeRestoreRequest() noexcept
            {
                return std::exchange(restore_, false);
            }

        private:
            bool restore_{};
            HistoryMenuActions menu_;
            lux::ui::CommandRouter &commands_;
            lux::ui::CommandHandle undo_, redo_;
            lux::ui::CommandRegistration undo_binding_, redo_binding_;
            bool valid_{};
            void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &) override
            {
                auto table = frame.table({lux::ui::WidgetIdView{"workbench-toolbar"}, 4, false, false, false});
                if (!table.visible())
                    return;
                table.nextColumn();
                frame.text("LUX / Scene Workbench");
                table.nextColumn();
                frame.textMuted("Live inspection | Read-only scene");
                table.nextColumn();
                if (frame.smallButton("Window history") && menu_.capture())
                    frame.openPopup(lux::ui::WidgetIdView{"window-history"});
                {
                    auto popup = frame.popup({lux::ui::WidgetIdView{"window-history"}, false});
                    if (popup.visible())
                    {
                        frame.textMuted("History target is fixed when this menu opens");
                        const auto item = [&](lux::ui::CommandHandle command, const char *label)
                        {
                            auto disabled = frame.disabled(!commands_.state(command).enabled);
                            if (frame.smallButton(label))
                            {
                                static_cast<void>(commands_.invoke(command));
                                popup.close();
                            }
                        };
                        item(undo_, "Undo");
                        item(redo_, "Redo");
                    }
                    else
                        static_cast<void>(menu_.cancel());
                }
                table.nextColumn();
                if (frame.smallButton("Restore panels / layout"))
                    restore_ = true;
            }
        };
    } // namespace
    struct SceneWorkspace::Impl final
    {
        const std::thread::id owner{std::this_thread::get_id()};
        EditorWindow &window;
        sessions::SceneSession &session;
        WorkspaceId identity;
        std::array<std::string, 5> pane_ids;
        WorkspaceLayout layout;
        std::unique_ptr<sessions::SceneView> view;
        std::unique_ptr<HistoryActions> history;
        std::unique_ptr<SceneViewport> viewport;
        std::unique_ptr<SceneOutliner> outliner;
        std::unique_ptr<SceneInspector> inspector;
        std::unique_ptr<SceneResourcesPane> resources;
        std::unique_ptr<SceneToolbar> toolbar;
        std::vector<lux::ui::PaneRegistration> registrations;
        std::vector<lux::ui::CommandRegistration> commands;
        HistoryTargetRegistration target;
        bool closing{}, detached{}, closed{true};
        Impl(EditorWindow &shell, WorkspaceId id, sessions::SceneSession &source)
            : window(shell), session(source), identity(id)
        {
            const auto prefix = "lux.scene.workspace." + std::to_string(id.value);
            pane_ids = {prefix + ".viewport", prefix + ".outliner", prefix + ".inspector", prefix + ".resources",
                        prefix + ".toolbar"};
            layout = sceneLayout(id, pane_ids);
        }
        std::array<lux::ui::Pane *, 5> panes() const noexcept
        {
            return {viewport.get(), outliner.get(), inspector.get(), resources.get(), toolbar.get()};
        }
        bool correctThread() const noexcept
        {
            return owner == std::this_thread::get_id();
        }
    };
    SceneWorkspace::SceneWorkspace(lux::object::ObjectDispatcherRef dispatcher, std::unique_ptr<Impl> impl)
        : Object(std::move(dispatcher)), impl_(std::move(impl))
    {
    }
    SceneWorkspace::~SceneWorkspace() noexcept
    {
        if (!impl_->closed)
            std::terminate();
    }
    WindowResult<std::unique_ptr<SceneWorkspace>> SceneWorkspace::create(
        EditorWindow &window, WorkspaceId id, sessions::SceneSession &session,
        std::unique_ptr<sessions::SceneView> &view) noexcept
    {
        if (!window.dispatcherRef().isCurrent())
            return fail(EWindowError::WRONG_THREAD);
        if (!id.value || !view || session.state() != sessions::ESessionState::READY)
            return fail(EWindowError::INVALID_ARGUMENT);
        if (view->sessionId() != session.id())
            return fail(EWindowError::INVALID_ARGUMENT);
        if (window.frameOpen())
            return fail(EWindowError::BUSY);
        try
        {
            auto impl = std::make_unique<Impl>(window, id, session);
            const auto dispatcher = window.uiSession().dispatcherRef();
            impl->history = std::make_unique<HistoryActions>(dispatcher, session);
            impl->viewport = std::make_unique<SceneViewport>(dispatcher, lux::ui::PaneId{impl->pane_ids[0]}, *view);
            impl->outliner = std::make_unique<SceneOutliner>(dispatcher, lux::ui::PaneId{impl->pane_ids[1]}, session);
            impl->inspector = std::make_unique<SceneInspector>(dispatcher, lux::ui::PaneId{impl->pane_ids[2]}, session);
            impl->resources =
                std::make_unique<SceneResourcesPane>(dispatcher, lux::ui::PaneId{impl->pane_ids[3]}, session);
            impl->toolbar = std::make_unique<SceneToolbar>(dispatcher, lux::ui::PaneId{impl->pane_ids[4]}, window);
            if (!impl->toolbar->valid())
                return fail(EWindowError::UI_FAILURE);
            impl->registrations.reserve(5);
            impl->commands.reserve(8);
            auto &router = window.uiSession().commandRouter();
            const auto undo = router.findCommand(lux::ui::UiCommandIdView{"lux.edit.undo"});
            const auto redo = router.findCommand(lux::ui::UiCommandIdView{"lux.edit.redo"});
            if (!undo || !redo)
                return fail(EWindowError::UI_FAILURE);
            for (auto *pane : impl->panes())
            {
                auto registration = window.uiSession().registerPane(*pane);
                if (!registration)
                    return fail(EWindowError::UI_FAILURE);
                impl->registrations.push_back(std::move(*registration));
                if (pane == impl->toolbar.get())
                    continue;
                auto undo_binding = router.bind<&HistoryActions::undo, &HistoryActions::canUndo>(
                    *undo, lux::ui::UiContextId{pane->id().name()}, *pane, *impl->history);
                if (!undo_binding)
                    return fail(EWindowError::UI_FAILURE);
                impl->commands.push_back(std::move(*undo_binding));
                auto redo_binding = router.bind<&HistoryActions::redo, &HistoryActions::canRedo>(
                    *redo, lux::ui::UiContextId{pane->id().name()}, *pane, *impl->history);
                if (!redo_binding)
                    return fail(EWindowError::UI_FAILURE);
                impl->commands.push_back(std::move(*redo_binding));
            }
            auto target = window.activeHistory().retainTarget(session);
            if (!target)
                return fail(EWindowError::UI_FAILURE);
            impl->target = std::move(*target);
            auto owner = std::unique_ptr<SceneWorkspace>(new SceneWorkspace(dispatcher, std::move(impl)));
            owner->impl_->view = std::move(view);
            owner->impl_->closed = false;
            return owner;
        }
        catch (const std::bad_alloc &)
        {
            return fail(EWindowError::ALLOCATION_FAILURE);
        }
    }
    WorkspaceId SceneWorkspace::id() const noexcept
    {
        return impl_->identity;
    }
    WindowResult<void> SceneWorkspace::activate() noexcept
    {
        if (!impl_->correctThread())
            return fail(EWindowError::WRONG_THREAD);
        if (impl_->closing || impl_->closed)
            return fail(EWindowError::CLOSED);
        if (impl_->window.frameOpen())
            return fail(EWindowError::BUSY);
        auto result = impl_->window.installLayout(impl_->layout);
        if (!result)
            return result;
        if (!impl_->window.activeHistory().activate(impl_->target.handle()))
            return fail(EWindowError::UI_FAILURE);
        static_cast<void>(impl_->window.uiSession().requestFocus(impl_->viewport->id().view()));
        return {};
    }
    SceneWorkspaceResult<void> SceneWorkspace::updateBeforeFrame() noexcept
    {
        if (!impl_->correctThread())
            return fail(EWindowError::WRONG_THREAD);
        if (impl_->closing || impl_->closed)
            return fail(EWindowError::CLOSED);
        if (impl_->window.frameOpen())
            return fail(EWindowError::BUSY);
        if (impl_->toolbar->takeRestoreRequest())
        {
            for (auto *pane : impl_->panes())
                pane->setVisible(true);
            auto result = impl_->window.installLayout(impl_->layout);
            if (!result)
                return lux::cxx::unexpected(SceneWorkspaceFailure{result.error()});
        }
        auto result = impl_->view->synchronize();
        if (!result)
            return lux::cxx::unexpected(SceneWorkspaceFailure{result.error()});
        return {};
    }
    WindowResult<void> SceneWorkspace::afterDraw(double seconds, lux::ui::Vec2 scale) noexcept
    {
        if (!impl_->correctThread())
            return fail(EWindowError::WRONG_THREAD);
        if (impl_->closing || impl_->closed)
            return fail(EWindowError::CLOSED);
        if (!impl_->window.frameOpen())
            return fail(EWindowError::BUSY);
        const auto input = impl_->window.uiSession().inputSnapshot();
        impl_->viewport->consumeInput(input, seconds, scale);
        return {};
    }
    std::span<const rendering::ViewImage> SceneWorkspace::frameImages() const noexcept
    {
        if (!impl_->correctThread())
            return {};
        return impl_->viewport ? impl_->viewport->frameImages() : std::span<const rendering::ViewImage>{};
    }
    void SceneWorkspace::releaseFrameImages() noexcept
    {
        if (!impl_->correctThread())
            return;
        if (impl_->viewport)
            impl_->viewport->releaseFrameImages();
    }
    WindowResult<void> SceneWorkspace::beginClose() noexcept
    {
        if (!impl_->correctThread())
            return fail(EWindowError::WRONG_THREAD);
        if (impl_->closed)
            return {};
        impl_->closing = true;
        return {};
    }
    SceneWorkspaceResult<sessions::ECloseProgress> SceneWorkspace::advanceClose() noexcept
    {
        if (!impl_->correctThread())
            return fail(EWindowError::WRONG_THREAD);
        if (impl_->closed)
            return sessions::ECloseProgress::COMPLETE;
        if (!impl_->closing)
            return fail(EWindowError::BUSY);
        if (impl_->window.frameOpen())
            return sessions::ECloseProgress::PENDING;
        if (!impl_->detached)
        {
            if (!impl_->target.reset())
                return fail(EWindowError::BUSY);
            impl_->commands.clear();
            impl_->registrations.clear();
            impl_->viewport->cancelCapture();
            releaseFrameImages();
            auto result = impl_->view->beginClose();
            if (!result)
                return lux::cxx::unexpected(SceneWorkspaceFailure{result.error()});
            impl_->detached = true;
        }
        auto result = impl_->view->advanceClose();
        if (!result)
            return lux::cxx::unexpected(SceneWorkspaceFailure{result.error()});
        if (*result == sessions::ECloseProgress::PENDING)
            return sessions::ECloseProgress::PENDING;
        impl_->toolbar.reset();
        impl_->resources.reset();
        impl_->inspector.reset();
        impl_->outliner.reset();
        impl_->viewport.reset();
        impl_->history.reset();
        impl_->view.reset();
        impl_->closed = true;
        return sessions::ECloseProgress::COMPLETE;
    }
} // namespace lux::editor::ui
