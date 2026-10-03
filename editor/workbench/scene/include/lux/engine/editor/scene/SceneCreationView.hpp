#pragma once
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>
#include <lux/engine/editor/sessions/SessionCommands.hpp>

namespace lux::editor::views { class ViewFactoryEntry; }

namespace lux::editor::scene
{
    struct SceneCreationRequests final
    {
        // The application creates/adopts the new Session. Rejection does not consume the form.
        std::function<SceneConfigurationResult<void>(const SceneCreationConfiguration&)> create;
    };
    class SceneCreationView final : public lux::ui::Pane
    {
    public:
        SceneCreationView(object::ObjectDispatcherRef, lux::ui::PaneId, SceneConfigurationInputs, SceneCreationRequests, SceneConfigurationResult<void>&);
        ~SceneCreationView() noexcept override;
        SceneCreationView(const SceneCreationView&) = delete;
        SceneCreationView& operator=(const SceneCreationView&) = delete;
        SceneCreationView(SceneCreationView&&) = delete;
        SceneCreationView& operator=(SceneCreationView&&) = delete;
        [[nodiscard]] SceneConfigurationElement& configuration() noexcept;
        void requestCreate() noexcept;
        [[nodiscard]] const SceneConfigurationResult<void>& status() const noexcept;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] SceneConfigurationResult<views::DetachedView> makeSceneCreationView(
        object::ObjectDispatcherRef,
        lux::ui::PaneId,
        SceneConfigurationInputs,
        SceneCreationRequests
    );
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> makeSceneCreationViewFactory(
        SceneConfigurationInputs, sessions::SessionCreation
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeNewSceneCommand(
        commands::CommandEntry::Query, desktop::ToolOpening
    );
}
