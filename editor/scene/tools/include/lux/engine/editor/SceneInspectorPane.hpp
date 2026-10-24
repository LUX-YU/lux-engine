#pragma once

#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::execution
{
    class ExecutionRuntime;
}

namespace lux::editor
{
    class ProjectScenes;
    class SceneDocument;

    // Displays owned author drafts. Applying a draft uses the document's revision-checked transaction.
    class SceneInspectorPane final : public ui::Pane
    {
    public:
        SceneInspectorPane(ProjectScenes&, execution::ExecutionRuntime&, std::string title);
        ~SceneInspectorPane() noexcept;

    private:
        void update() noexcept override;
        void waitForEdit(SceneDocument&) noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
