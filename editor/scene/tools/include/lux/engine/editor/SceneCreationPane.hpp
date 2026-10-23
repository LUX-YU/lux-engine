#pragma once

#include <lux/engine/ui/Pane.hpp>
#include <memory>

namespace lux::editor
{
    class EditorContext;
    class ProjectScenes;

    // Project catalog, explicit creation choices and opening. Runtime ownership stays in ProjectScenes.
    class SceneCreationPane final : public ui::Pane
    {
    public:
        SceneCreationPane(EditorContext&, ProjectScenes&, std::string title);
        ~SceneCreationPane() noexcept;

    private:
        void refreshCatalog() noexcept;
        void createScene() noexcept;
        void openScene() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
