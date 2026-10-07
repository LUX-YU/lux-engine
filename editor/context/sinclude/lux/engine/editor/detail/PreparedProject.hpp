#pragma once
#include <lux/engine/editor/ProjectDescription.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <memory>

namespace lux::project
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
        std::unique_ptr<project::PluginManager> plugins;
        std::shared_ptr<const project::SceneRegistrations> registrations;
    };
} // namespace lux::editor::detail
