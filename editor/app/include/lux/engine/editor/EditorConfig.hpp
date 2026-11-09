#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <lux/engine/core/execution/ExecutionConfig.hpp>
#include <lux/engine/editor/EditorLayout.hpp>
#include <lux/engine/plugins/PluginCatalog.hpp>

namespace lux::editor
{
    // Product locations; persisted manifests contain portable plugin selections only.
    struct ProjectPluginLocation final
    {
        std::filesystem::path catalog;
        std::filesystem::path root;
    };

    struct ProjectStartup final
    {
        std::optional<ProjectManifest> create;
        std::vector<plugins::PluginDescription> available_plugins;
    };

    struct EditorConfig final
    {
        std::string title{"EditorApplication"};
        int width{1280};
        int height{800};
        // Renderer/swapchain preference, never a CPU frame deadline.
        bool enable_vsync{true};
        ProjectLayout layout;
        std::vector<ProjectPluginLocation> plugin_locations;
        // One application's shared CPU pool and bounded blocking IO resource. No subsystem creates a second pool.
        execution::ExecutionRuntimeConfig execution{2, 512, {256}, execution::BlockingSchedulerConfig{2, 128}};
        bool enable_validation{false}; // Development diagnostics, independent of display pacing.
        // Empty opens the project selector. RUNNING always has a loaded project.
        std::filesystem::path project_file;
    };
} // namespace lux::editor
