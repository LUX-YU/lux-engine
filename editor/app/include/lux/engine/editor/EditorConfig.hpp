#pragma once
#include <filesystem>
#include <lux/engine/core/execution/ExecutionConfig.hpp>
#include <lux/engine/editor/EditorLayout.hpp>

namespace lux::editor
{
    // Product locations; persisted manifests contain portable plugin selections only.
    struct ProjectPluginLocation final
    {
        std::filesystem::path catalog;
        std::filesystem::path root;
    };

    struct EditorConfig final
    {
        std::string title{"LuxEngine"};
        int width{1280};
        int height{800};
        // Renderer/swapchain preference, never a CPU frame deadline.
        bool enable_vsync{true};
        EditorLayout layout;
        std::vector<ProjectPluginLocation> plugin_locations;
        // One application's shared CPU pool and bounded blocking IO resource. No subsystem creates a second pool.
        execution::ExecutionRuntimeConfig execution{2, 512, {256}, execution::BlockingSchedulerConfig{2, 128}};
        bool enable_validation{false}; // Development diagnostics, independent of display pacing.
        // A running editor always owns a loaded project. Empty is invalid for LuxEngine::create.
        std::filesystem::path project_file;
    };
} // namespace lux::editor
