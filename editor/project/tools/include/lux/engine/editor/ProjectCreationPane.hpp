#pragma once

#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::editor
{
    class LuxEngine;

    class ProjectCreationPane final : public ui::Pane
    {
    public:
        ProjectCreationPane(LuxEngine&, std::vector<ProjectManifest::PluginRecord> available_plugins);
        ~ProjectCreationPane() noexcept;

    private:
        void createProject() noexcept;
        void openProject() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
