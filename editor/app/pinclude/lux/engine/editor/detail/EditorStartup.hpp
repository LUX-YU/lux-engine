#pragma once

#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/editor/EditorAssembly.hpp>
#include <lux/engine/editor/EditorConfig.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>

namespace lux::execution
{
    class ExecutionRuntime;
}

namespace lux::editor
{
    class LuxEngine;
    class EditorWindow;
} // namespace lux::editor

namespace lux::editor::detail
{
    struct PreparedProject;
    // Initial intent only; no execution resources or project ownership.
    struct ProjectStartupRequest final
    {
        std::optional<ProjectManifest> create;
        std::vector<ProjectManifest::PluginRecord> available_plugins;
    };

    using StartupPump = cxx::function_ref<FrameworkResult<bool>() noexcept>;

    // Product-only interactive entry. Public LuxEngine::create requires a concrete project path.
    [[nodiscard]] FrameworkResult<std::unique_ptr<LuxEngine>> createEditor(
        EditorConfig,
        EditorAssembly,
        ProjectStartupRequest
    ) noexcept;

    // The shared frame driver keeps IPC/UI/completions moving while the blocking IO Sender prepares the project.
    [[nodiscard]] FrameworkResult<PreparedProject> prepareInitialProject(
        execution::ExecutionRuntime&,
        EditorWindow&,
        const EditorConfig&,
        ProjectStartupRequest,
        StartupPump
    ) noexcept;
} // namespace lux::editor::detail
