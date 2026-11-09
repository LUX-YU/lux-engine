#pragma once

#include <lux/engine/plugins/PluginCatalog.hpp>

#include <lux/engine/editor/ProjectEvents.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::editor
{
    // Pure metadata closure for project creation. It does not load executable plugin code.
    [[nodiscard]] scene::SceneContentAllowance
    availableProjectContent(
        std::span<const plugins::PluginDescription>,
        std::span<const ProjectManifest::PluginRecord>
    );

    class ProjectCreationPane final : public ui::Pane
    {
    public:
        // The composition root keeps the receiver alive through the Pane. This does not own a project/session.
        ProjectCreationPane(
            void* receiver,
            void (*open)(void*, OpenProjectRequest&) noexcept,
            void (*create)(void*, CreateProjectRequest&) noexcept,
            std::vector<plugins::PluginDescription> available_plugins,
            void (*action)(void*) noexcept = nullptr
        );
        void setStatus(std::string text) noexcept;
        // Coalesced presentation update through Root's existing structural safe point.
        void setApplicationState(EApplicationState) noexcept;
        ~ProjectCreationPane() noexcept;

    private:
        void applyState() noexcept;
        void createProject() noexcept;
        void refreshContent() noexcept;
        void openProject() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
