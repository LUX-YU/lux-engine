#pragma once
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/editor/storage/ProjectCreation.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::views { class ViewFactoryEntry; }

namespace lux::editor::project
{
    struct ProjectCreationDraft final
    {
        std::filesystem::path directory;
        std::string name, package;
        std::optional<scene::SceneCreationConfiguration> scene;
        std::vector<ProjectPluginEntry> plugins;
    };
    struct ProjectCreationConfiguration final
    {
        scene::SceneConfigurationInputs scene;
        std::vector<ProjectPluginEntry> plugins;
    };
    struct ProjectCreationProgress final
    {
        bool pending{}, launched{};
        std::optional<EditorFailure> failure;
        std::optional<ProjectCreationResult> committed;
    };
    // Synchronous admission/query only. The application owns accepted tasks and durable results.
    struct ProjectCreationRequests final
    {
        std::function<const lux::project::PluginCatalog*()> catalog;
        std::function<const ProjectCreationProgress&()> progress;
        std::function<EditorResult<void>(std::vector<ProjectPluginEntry>)> select;
        std::function<EditorResult<ProjectCreationConfiguration>()> configuration;
        std::function<EditorResult<void>(ProjectCreationDraft)> create;
        std::function<EditorResult<void>()> launch;
        std::function<void()> cancel;
        std::function<EditorResult<void>()> beginNew;
    };
    class ProjectCreationView final : public lux::ui::Pane
    {
    public:
        ProjectCreationView(object::ObjectDispatcherRef, lux::ui::PaneId, ProjectCreationRequests, EditorResult<void>&);
        ~ProjectCreationView() noexcept override;
        ProjectCreationView(const ProjectCreationView&) = delete;
        ProjectCreationView& operator=(const ProjectCreationView&) = delete;
        ProjectCreationView(ProjectCreationView&&) = delete;
        ProjectCreationView& operator=(ProjectCreationView&&) = delete;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> makeProjectCreationViewFactory(
        cxx::move_only_function<ProjectCreationRequests()> requests
    );
}
