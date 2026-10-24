#pragma once

#include <lux/engine/editor/detail/ProjectUiMount.hpp>
#include <memory>

namespace lux::editor
{
    class EditorContext;
    class ProjectScenes;
} // namespace lux::editor

namespace lux::editor::detail
{
    // One adopted project. Shared execution/render/window resources outlive this object.
    struct EditorProject final
    {
        explicit EditorProject(std::unique_ptr<EditorContext>) noexcept;
        ~EditorProject() noexcept;

        void beginClose() noexcept;
        [[nodiscard]] bool advanceClose() noexcept;

        std::unique_ptr<EditorContext> context;
        ProjectScenes* scenes{}; // Optional service, resolved at construction/close boundaries only.
        ProjectUiMount ui;
    };
} // namespace lux::editor::detail
