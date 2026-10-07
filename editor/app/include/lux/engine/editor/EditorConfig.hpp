#pragma once
#include <filesystem>
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
    };
} // namespace lux::editor
