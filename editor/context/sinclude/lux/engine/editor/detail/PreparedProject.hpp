#pragma once
#include <lux/engine/editor/EditorLayout.hpp>
#include <lux/engine/editor/ProjectDescription.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/plugins/PluginManager.hpp>
#include <memory>

namespace lux::plugins
{
    struct SceneRegistrations;
}

namespace lux::editor::detail
{
    // Worker-owned preparation. No UI, live Context or mutable registry is borrowed.
    struct PreparedProject final
    {
        ProjectDescription description;
        ProjectManifest manifest;
        std::unique_ptr<plugins::PluginManager> plugins;
        std::shared_ptr<const plugins::SceneRegistrations> registrations;
        std::optional<ProjectLayout> layout;
    };
} // namespace lux::editor::detail
