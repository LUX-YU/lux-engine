#include <lux/engine/editor/metadata/PluginLibrary.hpp>
#include <cassert>
#include <lux/engine/editor/DocumentRegistration.hpp>
#include <lux/engine/editor/detail/DocumentSource.hpp>
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/detail/SceneRun.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>

namespace lux::editor::scene
{
EditorResult<SceneEditorMetadata> sceneMetadata(std::span<const lux::simulation::ecs::ComponentSchema> additional,
    std::span<const std::shared_ptr<const PluginLibrary>> plugins)
{
    std::vector<lux::simulation::ecs::ComponentSchema> schemas;
    const auto append = [&schemas](auto values) { schemas.insert(schemas.end(), values.begin(), values.end()); };
    append(additional);
    for (const auto &plugin : plugins) append(plugin->components());
    auto set = lux::simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
    if (!set)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "component.schemas",
                                                  static_cast<std::uint64_t>(set.error().code)});
    }
    lux::simulation::SimulationSystemRegistry systems;
    for (const auto &plugin : plugins)
    {
        auto result = systems.add(plugin->simulationSystems());
        if (!result) return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "simulation.plugins"});
    }
    std::vector<lux::scene::SceneSystemRegistration> registrations;
    std::vector<lux::scene::RenderFeatureSceneBinding> loaded_bindings;
    for (const auto &plugin : plugins)
    {
        registrations.insert(registrations.end(), plugin->sceneSystems().begin(), plugin->sceneSystems().end());
        loaded_bindings.insert(loaded_bindings.end(), plugin->renderBindings().begin(), plugin->renderBindings().end());
    }
    std::vector<lux::render::RenderFeatureRegistration> loaded_features;
    for (const auto &plugin : plugins)
        loaded_features.insert(loaded_features.end(), plugin->renderFeatures().begin(), plugin->renderFeatures().end());
    return SceneEditorMetadata{std::move(*set),
        std::make_shared<const lux::simulation::SimulationSystemRegistry>(std::move(systems)),
        std::move(registrations), std::move(loaded_features), std::move(loaded_bindings)};
}

EditorResult<std::unique_ptr<lux::scene::SceneInstance>> detail::instantiateNativeScene(
    const NativeScene &source, const SceneEditorMetadata &metadata, process::TaskScope &tasks,
    lux::render::RenderRuntime &renderer, std::shared_ptr<lux::scene::RenderAssetSource> assets,
    lux::simulation::ESimulationMode mode, std::chrono::nanoseconds fixed_step)
{
    const bool author = mode == lux::simulation::ESimulationMode::DERIVATION;
    auto storage = source.storageSource();
    if (!storage)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::SOURCE_FAILURE, "scene.storage", 0, {}, storage.error()});
    }
    lux::scene::WorldLoadingServices loading{std::move(*storage), tasks};
    if (author)
    {
        // This desktop opens the complete author document. Gameplay keeps
        // the captured description's explicit bootstrap/Observer demands.
        for (std::uint32_t ordinal{}; ordinal < source.world->data().partitionCount(); ++ordinal)
        {
            loading.bootstrap.push_back({ordinal});
        }
    }
    lux::scene::RenderFeatureSceneBindings render_bindings = metadata.render_bindings;
    std::array providers{
        lux::scene::makeSceneCapabilityProvider<lux::render::RenderRuntime>("main-window", "lux.render.runtime",
                                                                            renderer),
        lux::scene::makeSceneCapabilityProvider<lux::scene::RenderFeatureSceneBindings>(
            "render-bindings", "lux.render.scene_bindings", render_bindings),
        lux::scene::makeSceneCapabilityProvider<std::shared_ptr<lux::scene::RenderAssetSource>>(
            "assets", "lux.render.assets", assets),
        lux::scene::makeSceneCapabilityProvider<lux::scene::WorldLoadingServices>("world-storage", "lux.world.loading",
                                                                                  loading)};
    EditorResult<std::shared_ptr<const lux::scene::SceneDescription>> description{
        std::shared_ptr<const lux::scene::SceneDescription>(source.scene, &source.scene->data())};
    if (!description)
    {
        return lux::cxx::unexpected(description.error());
    }
    auto created = lux::scene::SceneInstance::create(
        {std::move(*description),
         std::shared_ptr<const lux::world::WorldDescription>(source.world, &source.world->data()),
         std::shared_ptr<const lux::simulation::SimulationDescription>(source.simulation, &source.simulation->data()),
         metadata.components, *metadata.simulation_systems, metadata.scene_systems, providers, mode, fixed_step});
    if (!created)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                  "scene.create",
                                                  static_cast<std::uint64_t>(created.error().code),
                                                  {},
                                                  created.error()});
    }
    if (author)
    {
        auto *loading_system = (*created)->findSceneSystem<lux::scene::WorldLoadingSystem>();
        if (!loading_system)
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.world-loading.required"});
        auto materialized = loading_system->adoptCaptured(source.partitions);
        if (!materialized)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                      "world.materialize",
                                                      static_cast<std::uint64_t>(materialized.error().code),
                                                      {},
                                                      materialized.error()});
        }
    }
    const auto sealed = (*created)->simulation().seal();
    if (!sealed)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::SOURCE_FAILURE, "simulation.seal", 0, {}, sealed.error()});
    }
    return std::move(*created);
}

namespace
{
struct SceneCodec final
{
    using Source = NativeScene;
    static constexpr std::size_t max_bytes = 256U * 1024U * 1024U;
    lux::render::RenderRuntime &renderer;
    SceneEditorMetadata metadata;
    std::shared_ptr<detail::SceneRunSlot> run_slot;
    std::shared_ptr<detail::SceneAssetSources> asset_sources;

    static lux::asset::AssetId identity(const Source &source) noexcept
    {
        return source.scene->id();
    }
    static EditorResult<Source> decode(const lux::cxx::SharedBytes<> &bytes, std::stop_token stop)
    {
        auto source = decodeNativeScene(bytes, stop);
        if (!source)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                      "scene.codec",
                                                      static_cast<std::uint64_t>(source.error().code),
                                                      {},
                                                      source.error()});
        }
        return std::move(*source);
    }
    std::unique_ptr<SceneEditor> document;

    EditorResult<bool> prepare(Source &source, Project &project, process::ExecutionRuntime &runtime,
                               std::stop_token stop, PollBudget &)
    {
        if (stop.stop_requested())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "scene.open"});
        }
        auto opened = SceneEditor::open(source, project, runtime, renderer, metadata, asset_sources, run_slot);
        if (!opened)
        {
            return lux::cxx::unexpected(opened.error());
        }
        document = std::move(*opened);
        return true;
    }

    EditorResult<std::unique_ptr<DocumentEditor>> adopt(Source &, Project &, process::ExecutionRuntime &)
    {
        return std::unique_ptr<DocumentEditor>(std::move(document));
    }
};
} // namespace

EditorResult<std::shared_ptr<lux::scene::RenderAssetSource>> detail::SceneAssetSources::acquire(Project &project)
{
    // Project instance identity rejects reopened projects even if the same
    // asset catalog occupies the same address. An old Run retains its version.
    const auto identity = project.reference({});
    std::erase_if(records_, [](const auto &record) { return record.source.expired(); });
    for (const auto &record : records_)
    {
        if (record.project == identity.project_instance && record.revision == identity.catalog_revision)
        {
            if (auto source = record.source.lock())
            {
                return source;
            }
        }
    }

    auto reads = project.captureAssetReads();
    if (!reads)
    {
        return lux::cxx::unexpected(reads.error());
    }
    auto source = std::make_shared<lux::scene::RenderAssetSource>(runtime_, project.tasks(), std::move(*reads),
                                                                  identity.catalog_revision);
    records_.push_back({identity.project_instance, identity.catalog_revision, source});
    return source;
}

DocumentRegistration sceneDocumentRegistration(process::ExecutionRuntime &runtime, lux::render::RenderRuntime &renderer,
                                               SceneEditorMetadata metadata)
{
    return {
        std::string(kSceneDocumentType),
        [&runtime, &renderer, metadata = std::move(metadata), run_slot = std::make_shared<detail::SceneRunSlot>(),
         assets = std::make_shared<detail::SceneAssetSources>(renderer)](
            Project &project, const OpenDocumentRequest &request) -> EditorResult<std::unique_ptr<DocumentOpening>> {
            const auto *entry = project.asset(request.key.source);
            const bool valid = request.key.project == project.manifest().id && request.key.type == kSceneDocumentType;
            if (!entry || entry->kind != EProjectAssetKind::SCENE || !runtime.blocking() || !metadata.simulation_systems ||
                !valid)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.open"});
            }
            return std::unique_ptr<DocumentOpening>(new lux::editor::detail::SourceOpening<SceneCodec>(
                project, *entry, runtime, SceneCodec{renderer, metadata, run_slot, assets}));
        }};
}
} // namespace lux::editor::scene
