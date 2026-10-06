#pragma once
#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/editor/storage/ProjectCreation.hpp>
#include <lux/engine/services/ServiceDescriptor.hpp>

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
    struct ProjectCreationOptions final
    {
        std::filesystem::path installation;
        bool launch_created{true};
    };
    extern const services::ServiceDescriptor kProjectCreationService;

    // One bounded creation use case, shared by Editor and Launcher. No Pane owns its accepted work.
    // Views share this scoped owner; closing a view does not discard admitted work or published facts.
    class ProjectCreation final
    {
    public:
        ProjectCreation(
            process::ExecutionRuntime&,
            object::ObjectDispatcherRef,
            std::filesystem::path installation,
            bool launch_created = true
        );
        ~ProjectCreation() noexcept;
        ProjectCreation(const ProjectCreation&) = delete;
        ProjectCreation& operator=(const ProjectCreation&) = delete;
        ProjectCreation(ProjectCreation&&) = delete;
        ProjectCreation& operator=(ProjectCreation&&) = delete;
        [[nodiscard]] EditorResult<void> start();
        [[nodiscard]] const lux::project::PluginCatalog* catalog() const noexcept;
        [[nodiscard]] EditorResult<void> select(std::vector<ProjectPluginEntry>);
        [[nodiscard]] EditorResult<ProjectCreationConfiguration> configuration();
        [[nodiscard]] EditorResult<void> create(ProjectCreationDraft);
        [[nodiscard]] EditorResult<void> launch();
        [[nodiscard]] EditorResult<void> beginNew();
        [[nodiscard]] const ProjectCreationProgress& progress() const noexcept;
        void update();
        void cancel() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::project
