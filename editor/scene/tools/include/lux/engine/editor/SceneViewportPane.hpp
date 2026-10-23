#pragma once

#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::scene
{
    class SceneRuntime;
}

namespace lux::editor
{
    class ProjectScenes;

    class SceneViewportPane final : public ui::Pane
    {
    public:
        SceneViewportPane(ProjectScenes&, scene::SceneRuntime&, std::string title);
        ~SceneViewportPane() noexcept;

    private:
        void update() noexcept override;
        void refreshDocuments() noexcept;
        void selectDocument() noexcept;
        void refreshCameras() noexcept;
        void updateCamera() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
