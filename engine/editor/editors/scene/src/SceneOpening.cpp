#include <cassert>
#include <lux/engine/editor/DocumentRegistration.hpp>
#include <lux/engine/editor/detail/DocumentSource.hpp>
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/detail/SceneRun.hpp>
#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <lux/engine/function/render/features/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/HighlightOperation.ops.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/TransformSystem.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>

namespace lux::editor::scene
{
EditorResult<SceneEditorMetadata> sceneMetadata(std::span<const lux::simulation::ecs::ComponentSchema> additional)
{
    lux::meta::ReflectionRegistry::drainPending();
    lux::scene::initializeBuiltinRenderSystemMeta();
    lux::render::initializeBuiltinRenderFeatureMeta();
    std::vector<lux::simulation::ecs::ComponentSchema> schemas;
    const auto append = [&schemas](auto values) { schemas.insert(schemas.end(), values.begin(), values.end()); };
    append(lux::simulation::ecs::transformComponentSchemas());
    append(lux::simulation::ecs::hierarchyComponentSchemas());
    append(lux::simulation::ecs::visualComponentSchemas());
    append(lux::scene::sceneRenderComponentSchemas());
    append(lux::scene::worldLoadingComponentSchemas());
    append(additional);
    auto set = lux::simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
    if (!set)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "component.schemas",
                                                  static_cast<std::uint64_t>(set.error().code)});
    }
    lux::simulation::SimulationSystemRegistry systems;
    const auto added = systems.add(lux::simulation::transformSystemRegistrations());
    if (!added)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "simulation.registry",
                                                  static_cast<std::uint64_t>(added.error().code)});
    }
    const auto render = lux::scene::builtinRenderSystemRegistrations();
    const auto features = lux::render::builtinRenderFeatureRegistrations();
    const auto bindings = lux::scene::builtinRenderFeatureSceneBindings();
    std::vector<lux::scene::SceneSystemRegistration> registrations(render.begin(), render.end());
    registrations.push_back(lux::scene::builtinMeshQuerySystemRegistration());
    registrations.push_back(lux::scene::worldLoadingSystemRegistration());
    auto metadata =
        lux::scene::SceneMetaManager::build({std::move(*set), std::move(systems), std::move(registrations)});
    if (!metadata)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.metadata",
                                                  static_cast<std::uint64_t>(metadata.error().code)});
    }
    auto render_metadata =
        lux::scene::RenderSystemMetadata::build(*metadata, {features.begin(), features.end()}, bindings);
    if (!render_metadata)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "render.metadata",
                                                  static_cast<std::uint64_t>(render_metadata.error().code)});
    }
    return SceneEditorMetadata{std::make_shared<const lux::scene::SceneMetaManager>(std::move(*metadata)),
                               std::make_shared<const lux::scene::RenderSystemMetadata>(std::move(*render_metadata))};
}

EditorResult<std::shared_ptr<const lux::scene::SceneDescription>> detail::editorSceneDescription(
    const NativeScene &source, const SceneEditorMetadata &metadata, bool author_view)
{
    const auto &description = source.scene->data();
    const auto type = lux::scene::builtinMeshQuerySystemRegistration().type;
    bool has_query{}, has_loading{};
    lux::system::SystemInstanceId query_id{};
    std::vector<lux::system::SystemInstanceId> render_systems;
    const auto loading = lux::scene::worldLoadingSystemRegistration();
    std::uint64_t next{1};
    for (std::size_t index{}; index < description.systemCount(); ++index)
    {
        const auto system = description.systemAt(index);
        has_query |= system.type() == type;
        if (system.type() == type)
        {
            query_id = system.instanceId();
        }
        if (system.type() == lux::scene::builtinRenderSystemRegistration().type)
        {
            render_systems.push_back(system.instanceId());
        }
        has_loading |= system.type() == loading.type;
        next = (std::max)(next, system.instanceId().value);
    }
    const auto schemas = source.world->data().schemas();
    const bool spatial =
        std::ranges::find(schemas, "lux.ecs.Transform3D", &lux::world::WorldDataSchemaId::name) != schemas.end();
    const auto additions = static_cast<std::uint64_t>(!has_loading) + (!has_query && spatial);
    if (next > UINT64_MAX - additions)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "scene.query.install"});
    }
    // Editor-only capability composition. The immutable source description is preserved on save/export.
    lux::scene::SceneDescriptionBuilder builder;
    builder.setWorld(description.world());
    builder.setSimulation(description.simulation());
    lux::cxx::expected<void, lux::scene::SceneDescriptionFailure> result;
    for (std::size_t index{}; result && index < description.systemCount(); ++index)
    {
        const auto system = description.systemAt(index);
        std::vector<std::byte> configuration;
        auto payload = system.configurationPayload();
        if (system.type() == lux::scene::builtinRenderSystemRegistration().type)
        {
            const auto registration = lux::scene::builtinRenderSystemRegistration();
            lux::scene::RenderSystemConfiguration config;
            auto decoded = registration.configuration.decode(payload, &config);
            if (!decoded)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "scene.render.config", 0, {}, decoded.error()});
            }
            const auto append = [&](lux::render::FeatureTypeId feature) {
                if (std::ranges::find(config.features, feature, &lux::scene::RenderFeatureInstanceDescription::type) ==
                    config.features.end())
                {
                    const auto *meta = metadata.render->feature(feature);
                    if (meta)
                    {
                        config.features.push_back(
                            {feature, {meta->default_configuration.begin(), meta->default_configuration.end()}});
                    }
                }
            };
            if (std::ranges::find(schemas, "lux.ecs.Mesh3D", &lux::world::WorldDataSchemaId::name) != schemas.end())
            {
                append(lux::render::kHighlightRenderFeatureRegistration.descriptor->type);
            }
            if (author_view && spatial)
            {
                append(lux::render::kGrid3DRenderFeatureRegistration.descriptor->type);
            }
            auto encoded = registration.configuration.encode(&config, configuration);
            if (!encoded)
            {
                return lux::cxx::unexpected(
                    EditorFailure{EEditorError::SOURCE_FAILURE, "scene.render.config", 0, {}, encoded.error()});
            }
            payload = configuration;
        }
        result = builder.addSystem(system.instanceId(), system.instanceName(), system.type(), system.version(),
                                   system.configurationSchemaName(), system.configurationSchemaVersion(), payload);
        for (std::size_t binding{}; result && binding < system.requirementBindingCount(); ++binding)
        {
            const auto value = system.requirementBindingAt(binding);
            result = builder.bindRequirement(system.instanceId(), value.requirement(), value.provider());
        }
    }
    for (std::size_t index{}; result && index < description.dependencyCount(); ++index)
    {
        const auto dependency = description.dependencyAt(index);
        result = builder.addDependency(dependency.before(), dependency.after());
    }
    if (result && !has_query && spatial)
    {
        query_id = {++next};
        result = builder.addSystem(query_id, "editor.mesh-query", type, 1, {}, 0);
    }
    if (result && query_id.value)
    {
        for (const auto render : render_systems)
        {
            bool declared{};
            for (std::size_t index{}; index < description.dependencyCount(); ++index)
            {
                const auto edge = description.dependencyAt(index);
                declared |= edge.before() == query_id && edge.after() == render;
            }
            if (!declared)
            {
                result = builder.addDependency(query_id, render);
                if (!result)
                {
                    break;
                }
            }
        }
    }
    if (result && !has_loading)
    {
        // Existing complete native documents explicitly demand their whole
        // source. A declared loading system keeps its own bootstrap policy.
        lux::scene::WorldLoadingConfiguration config;
        for (std::uint32_t ordinal{}; ordinal < source.world->data().partitionCount(); ++ordinal)
        {
            config.bootstrap.push_back({ordinal});
        }
        std::vector<std::byte> payload;
        auto encoded = loading.configuration.encode(&config, payload);
        if (!encoded)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "scene.loading.configuration", 0, {}, encoded.error()});
        }
        result = builder.addSystem({++next}, "world-loading", loading.type, loading.description->version,
                                   loading.description->configuration_schema_name,
                                   loading.description->configuration_schema_version, payload);
    }
    if (!result)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::SOURCE_FAILURE, "scene.query.install", 0, {}, result.error()});
    }
    auto built = std::move(builder).build();
    if (!built)
    {
        return lux::cxx::unexpected(
            EditorFailure{EEditorError::SOURCE_FAILURE, "scene.query.install", 0, {}, built.error()});
    }
    return std::make_shared<const lux::scene::SceneDescription>(std::move(*built));
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
    auto render_metadata = metadata.render;
    std::array providers{
        lux::scene::makeSceneCapabilityProvider<lux::render::RenderRuntime>("main-window", "lux.render.runtime",
                                                                            renderer),
        lux::scene::makeSceneCapabilityProvider<std::shared_ptr<const lux::scene::RenderSystemMetadata>>(
            "render-meta", "lux.render.metadata", render_metadata),
        lux::scene::makeSceneCapabilityProvider<std::shared_ptr<lux::scene::RenderAssetSource>>(
            "assets", "lux.render.assets", assets),
        lux::scene::makeSceneCapabilityProvider<lux::scene::WorldLoadingServices>("world-storage", "lux.world.loading",
                                                                                  loading)};
    auto description = editorSceneDescription(source, metadata, author);
    if (!description)
    {
        return lux::cxx::unexpected(description.error());
    }
    auto created = lux::scene::SceneInstance::create(
        {std::move(*description),
         std::shared_ptr<const lux::world::WorldDescription>(source.world, &source.world->data()),
         std::shared_ptr<const lux::simulation::SimulationDescription>(source.simulation, &source.simulation->data()),
         *metadata.scene, providers, mode, fixed_step});
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
        assert(loading_system);
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
            if (!entry || entry->kind != EProjectAssetKind::SCENE || !runtime.blocking() || !metadata.scene ||
                !metadata.render || !valid)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.open"});
            }
            return std::unique_ptr<DocumentOpening>(new lux::editor::detail::SourceOpening<SceneCodec>(
                project, *entry, runtime, SceneCodec{renderer, metadata, run_slot, assets}));
        }};
}
} // namespace lux::editor::scene
