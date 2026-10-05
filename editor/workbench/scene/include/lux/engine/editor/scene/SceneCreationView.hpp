#pragma once
namespace lux::editor::views
{
    class ViewFactoryEntry;
    class DetachedView;
    struct ViewFactoryDescriptor;
} // namespace lux::editor::views
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/editor/sessions/SessionCommands.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::desktop
{
    struct UiDescriptor;
}

namespace lux::editor::views
{
    class ViewFactoryEntry;
}

namespace lux::editor::scene
{
    extern const desktop::UiDescriptor kSceneCreationView;

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
        SceneConfigurationInputs,
        sessions::SessionCreation
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makeNewSceneCommand(
        commands::CommandEntry::Query,
        desktop::ToolOpening
    );
} // namespace lux::editor::scene
