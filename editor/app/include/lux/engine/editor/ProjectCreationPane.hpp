#pragma once

#include <lux/engine/editor/ProjectEvents.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::editor
{
    class ProjectCreationPane final : public ui::Pane
    {
    public:
        // The composition root keeps the receiver alive through the Pane. This does not own a project/session.
        ProjectCreationPane(
            void* receiver,
            void (*open)(void*, OpenProjectRequest&) noexcept,
            void (*create)(void*, CreateProjectRequest&) noexcept,
            std::vector<ProjectManifest::PluginRecord> available_plugins
        );
        void setStatus(std::string text) noexcept;
        ~ProjectCreationPane() noexcept;

    private:
        void createProject() noexcept;
        void openProject() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
