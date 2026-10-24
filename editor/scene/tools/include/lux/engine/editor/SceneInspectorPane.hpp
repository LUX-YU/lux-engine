#pragma once

#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::editor
{
    class ProjectScenes;

    // Displays owned author drafts. Applying a draft uses the document's revision-checked transaction.
    class SceneInspectorPane final : public ui::Pane
    {
    public:
        SceneInspectorPane(ProjectScenes&, std::string title);
        ~SceneInspectorPane() noexcept;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
