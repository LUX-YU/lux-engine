#pragma once

#include <lux/engine/editor/metadata/EditorPluginExports.hpp>
#include <lux/engine/editor/metadata/EngineMetadata.hpp>
#include <lux/engine/scene/RenderScenePluginExports.hpp>
#include <lux/engine/scene/ScenePluginExports.hpp>
#include <lux/engine/simulation/SimulationPluginExports.hpp>
#include <lux/engine/simulation/ecs/ComponentPluginExports.hpp>
#include <lux/engine/function/render/client/RenderPluginExports.hpp>

namespace lux::editor
{
[[nodiscard]] LUX_EDITOR_METADATA_PUBLIC std::string_view pluginSdkAbi() noexcept;

// Owns executable resources and verified tables. Admission/publication belongs to
// the caller; loading this object does not modify any live registry or Renderer.
class LUX_EDITOR_METADATA_PUBLIC PluginLibrary final
{
  public:
    [[nodiscard]] static PluginResult<std::shared_ptr<const PluginLibrary>> load(
        const PluginDescription &,
        std::span<const std::shared_ptr<const PluginLibrary>> dependencies = {}) noexcept;
    ~PluginLibrary();
    PluginLibrary(const PluginLibrary &) = delete;
    PluginLibrary &operator=(const PluginLibrary &) = delete;

    [[nodiscard]] const MetadataIdentity &identity() const noexcept { return identity_; }
    [[nodiscard]] std::shared_ptr<const void> runtimeCode() const noexcept { return runtime_; }
    [[nodiscard]] std::shared_ptr<const void> editorCode() const noexcept { return editor_; }
    [[nodiscard]] std::span<const simulation::SimulationSystemRegistration> simulationSystems() const noexcept { return simulation_; }
    [[nodiscard]] std::span<const scene::SceneSystemRegistration> sceneSystems() const noexcept { return scene_; }
    [[nodiscard]] std::span<const simulation::ecs::ComponentSchema> components() const noexcept { return components_; }
    [[nodiscard]] std::span<const render::RenderFeatureRegistration> renderFeatures() const noexcept { return features_; }
    [[nodiscard]] std::span<const scene::RenderFeatureSceneBinding> renderBindings() const noexcept { return bindings_; }
    [[nodiscard]] const EditorPluginExports *editorExports() const noexcept { return tools_; }

  private:
    PluginLibrary() = default;
    MetadataIdentity identity_;
    std::shared_ptr<const void> runtime_, editor_;
    std::vector<simulation::SimulationSystemRegistration> simulation_;
    std::vector<scene::SceneSystemRegistration> scene_;
    std::vector<simulation::ecs::ComponentSchema> components_;
    std::vector<render::RenderFeatureRegistration> features_;
    std::vector<scene::RenderFeatureSceneBinding> bindings_;
    const EditorPluginExports *tools_{};
};
} // namespace lux::editor
