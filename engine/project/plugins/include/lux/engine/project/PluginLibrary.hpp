#pragma once

#include <lux/engine/project/PluginCatalog.hpp>
#include <lux/engine/scene/ScenePluginExports.hpp>
#include <lux/engine/simulation/SimulationPluginExports.hpp>
#include <lux/engine/simulation/ecs/ComponentPluginExports.hpp>

namespace lux::engine::platform
{
    class DynamicLibrary;
}

namespace lux::project
{
    [[nodiscard]] std::string_view pluginSdkAbi() noexcept;

    // Loads and verifies one declared binary, retaining dependency modules until it unloads.
    // The caller interprets the export table for its own layer.
    [[nodiscard]] PluginResult<std::shared_ptr<const engine::platform::DynamicLibrary>> loadPluginLibrary(
        const PluginDescription& plugin,
        const PluginLibraryDescription& library,
        std::span<const std::shared_ptr<const void>> dependencies = {}
    ) noexcept;

    // Owns executable resources and verified tables. Admission/publication belongs to
    // the caller; loading this object does not modify any live registry or Renderer.
    class PluginLibrary final
    {
    public:
        [[nodiscard]] static PluginResult<std::shared_ptr<const PluginLibrary>> load(
            const PluginDescription&,
            std::span<const std::shared_ptr<const PluginLibrary>> dependencies = {}
        ) noexcept;
        ~PluginLibrary();
        PluginLibrary(const PluginLibrary&) = delete;
        PluginLibrary& operator=(const PluginLibrary&) = delete;

        [[nodiscard]] const MetadataIdentity& identity() const noexcept
        {
            return description_.identity;
        }
        [[nodiscard]] std::shared_ptr<const void> runtimeCode() const noexcept
        {
            return runtime_;
        }
        [[nodiscard]] std::span<const simulation::SimulationSystemRegistration> simulationSystems() const noexcept
        {
            return simulation_;
        }
        [[nodiscard]] std::span<const lux::scene::SceneSystemRegistration> sceneSystems() const noexcept
        {
            return scene_;
        }
        [[nodiscard]] std::span<const simulation::ecs::ComponentSchema> components() const noexcept
        {
            return components_;
        }
        [[nodiscard]] const PluginDescription& description() const noexcept
        {
            return description_;
        }
        [[nodiscard]] const engine::platform::DynamicLibrary& library() const noexcept
        {
            return *runtime_;
        }

    private:
        PluginLibrary() = default;
        PluginDescription description_;
        std::shared_ptr<const engine::platform::DynamicLibrary> runtime_;
        std::vector<simulation::SimulationSystemRegistration> simulation_;
        std::vector<lux::scene::SceneSystemRegistration> scene_;
        std::vector<simulation::ecs::ComponentSchema> components_;
    };
} // namespace lux::project
