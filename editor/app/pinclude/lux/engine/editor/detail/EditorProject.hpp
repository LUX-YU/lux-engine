#pragma once

#include <exec/task.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/ProjectEvents.hpp>
#include <lux/engine/editor/detail/ProjectUiMount.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <memory>
#include <optional>

namespace lux::editor
{
    class EditorContext;
}

namespace lux::editor::detail
{
    // Shared application facilities outlive the project and its asynchronous close.
    struct EditorProject final
    {
        explicit EditorProject(std::unique_ptr<EditorContext>) noexcept;
        ~EditorProject() noexcept;
        [[nodiscard]] exec::task<FrameworkResult<void>> close() noexcept;
        [[nodiscard]] exec::task<FrameworkResult<void>> recoverClose() noexcept;
        std::unique_ptr<EditorContext> context;
        ProjectUiMount ui;
    };

    // Finite transaction data, not an execution owner or another project registry.
    struct ProjectChange final
    {
        std::uint64_t request{};
        std::filesystem::path manifest_file;
        std::optional<ProjectManifest> create;
        EManifestPublication manifest_publication{EManifestPublication::UNCHANGED};
        std::unique_ptr<EditorProject> candidate;
        std::unique_ptr<EditorProject> previous;
        std::vector<std::unique_ptr<ui::Pane>> panes;
        ui::PanePlacement placement;
        error::Error preparation_failure;
        error::Error cleanup_failure;
        bool project_published = false;
    };
} // namespace lux::editor::detail
