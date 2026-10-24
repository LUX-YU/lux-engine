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

    // Revision-derived rows only; SceneDocument/ECS remain the sole author hierarchy.
    class SceneHierarchyPane final : public ui::Pane
    {
    public:
        SceneHierarchyPane(ProjectScenes&, execution::ExecutionRuntime&, std::string title);
        ~SceneHierarchyPane() noexcept;

    private:
        void update() noexcept override;
        void waitForEdit(SceneDocument&) noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
