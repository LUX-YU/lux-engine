#pragma once

#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::editor
{
    class ProjectScenes;

    // Revision-derived rows only; SceneDocument/ECS remain the sole author hierarchy.
    class SceneHierarchyPane final : public ui::Pane
    {
    public:
        SceneHierarchyPane(ProjectScenes&, std::string title);
        ~SceneHierarchyPane() noexcept;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
