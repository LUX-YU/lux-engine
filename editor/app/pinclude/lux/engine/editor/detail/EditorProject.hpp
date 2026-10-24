#pragma once

#include <exec/task.hpp>
#include <lux/engine/editor/detail/ProjectUiMount.hpp>
#include <memory>
#include <optional>

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

        // Start teardown on Main. No pending Scene/producer borrow means immediate destruction is safe;
        // otherwise the caller keeps this project until the returned continuation finishes.
        [[nodiscard]] std::optional<exec::task<void>> close() noexcept;

        std::unique_ptr<EditorContext> context;
        ProjectScenes* scenes{}; // Optional service, resolved at construction/close boundaries only.
        ProjectUiMount ui;

    private:
        [[nodiscard]] exec::task<void> completeClose() noexcept;
    };
} // namespace lux::editor::detail
