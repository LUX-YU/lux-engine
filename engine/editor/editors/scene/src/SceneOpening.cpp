#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/detail/DocumentSource.hpp>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/detail/SceneRun.hpp>
#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <lux/engine/function/render/features/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/HighlightOperation.ops.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneRenderBinding.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
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
        auto metadata = lux::scene::SceneMetaManager::build({std::move(*set), std::move(systems), std::move(registrations)});
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
        return SceneEditorMetadata{
            std::make_shared<const lux::scene::SceneMetaManager>(std::move(*metadata)),
            std::make_shared<const lux::scene::RenderSystemMetadata>(std::move(*render_metadata))};
    }

    EditorResult<std::shared_ptr<const lux::scene::SceneDescription>> detail::editorSceneDescription(const NativeScene &source)
    {
        const auto &description = source.scene->data();
        const auto type = lux::scene::builtinMeshQuerySystemRegistration().type;
        bool has_query{};
        std::uint64_t next{1};
        for (std::size_t index{}; index < description.systemCount(); ++index)
        {
            const auto system = description.systemAt(index);
            has_query |= system.type() == type;
            next = (std::max)(next, system.instanceId().value);
        }
        const auto schemas = source.world->data().schemas();
        const bool spatial = std::ranges::find(schemas, "lux.ecs.Transform3D", &lux::world::WorldDataSchemaId::name) != schemas.end();
        if (has_query || !spatial)
        {
            return std::shared_ptr<const lux::scene::SceneDescription>(source.scene, &description);
        }
        if (next == UINT64_MAX)
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
            result = builder.addSystem(system.instanceId(), system.instanceName(), system.type(), system.version(),
                system.configurationSchemaName(), system.configurationSchemaVersion(), system.configurationPayload());
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
        if (result)
        {
            result = builder.addSystem({next + 1}, "editor.mesh-query", type, 1, {}, 0);
        }
        if (!result)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.query.install", 0, {}, result.error()});
        }
        auto built = std::move(builder).build();
        if (!built)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.query.install", 0, {}, built.error()});
        }
        return std::make_shared<const lux::scene::SceneDescription>(std::move(*built));
    }

    EditorResult<std::unique_ptr<lux::scene::SceneRenderBinding>> detail::beginSceneRendering(
        rendering::EditorRenderer &renderer, const NativeScene &source, const SceneEditorMetadata &metadata,
        bool author_view)
    {
        const auto &description = source.scene->data();
        for (std::size_t index{}; index < description.systemCount(); ++index)
        {
            const auto system = description.systemAt(index);
            if (system.type() != lux::scene::builtinRenderSystemRegistration().type)
            {
                continue;
            }
            // Visual editor contributions do not alter the persisted Scene description.
            std::array<lux::render::FeatureTypeId, 2> features{};
            std::size_t feature_count{};
            const auto schemas = source.world->data().schemas();
            if (std::ranges::find(schemas, "lux.ecs.Mesh3D", &lux::world::WorldDataSchemaId::name) != schemas.end())
            {
                features[feature_count++] = lux::render::kHighlightRenderFeatureRegistration.descriptor->type;
            }
            if (author_view && std::ranges::find(schemas, "lux.ecs.Transform3D",
                                                 &lux::world::WorldDataSchemaId::name) != schemas.end())
            {
                features[feature_count++] = lux::render::kGrid3DRenderFeatureRegistration.descriptor->type;
            }
            auto started = lux::scene::SceneRenderBinding::begin(
                renderer, system, metadata.render,
                std::span<const lux::render::FeatureTypeId>{features}.first(feature_count));
            if (!started)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                          "scene.render.begin",
                                                          static_cast<std::uint64_t>(started.error().scene.code),
                                                          {},
                                                          started.error()});
            }
            return std::move(*started);
        }
        return std::unique_ptr<lux::scene::SceneRenderBinding>{};
    }

    namespace
    {
        struct SceneCodec final
        {
            using Source = NativeScene;
            static constexpr std::size_t max_bytes = 256U * 1024U * 1024U;
            rendering::EditorRenderer &renderer;
            SceneEditorMetadata metadata;
            std::shared_ptr<detail::SceneRunSlot> run_slot;

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
            std::unique_ptr<lux::scene::SceneRenderBinding> binding;
            std::unique_ptr<SceneEditor> document;
            std::variant<std::monostate, EditorFailure> failure;
            bool initialized{};

            EditorResult<bool> prepare(Source &source, Project &project, process::ExecutionRuntime &runtime,
                                       std::stop_token stop, PollBudget &budget)
            {
                using State = lux::scene::ESceneRenderBindingState;
                if (stop.stop_requested())
                {
                    failure = EditorFailure{EEditorError::CANCELLED, "scene.open"};
                }
                if (!initialized && failure.index() == 0)
                {
                    initialized = true;
                    auto started = detail::beginSceneRendering(renderer, source, metadata);
                    if (!started)
                    {
                        failure = std::move(started.error());
                    }
                    else
                    {
                        binding = std::move(*started);
                    }
                    if (!binding && failure.index() == 0)
                    {
                        auto opened =
                            SceneEditor::open(source, project, runtime, renderer, metadata, nullptr, binding, run_slot);
                        if (!opened)
                        {
                            failure = opened.error();
                        }
                        else
                        {
                            document = std::move(*opened);
                        }
                    }
                }
                if (binding)
                {
                    budget.render_programs -= binding->poll(budget.render_programs);
                    if (binding->hasFailure() && failure.index() == 0)
                    {
                        failure = EditorFailure{EEditorError::SOURCE_FAILURE,
                                                "scene.render.prepare",
                                                static_cast<std::uint64_t>(binding->failure().scene.code),
                                                {},
                                                binding->failure()};
                    }
                    if (failure.index() == 0 && binding->state() == State::READY)
                    {
                        auto input = binding->takeInput();
                        if (!input)
                        {
                            failure = EditorFailure{EEditorError::SOURCE_FAILURE,
                                                    "scene.render.input",
                                                    static_cast<std::uint64_t>(input.error().scene.code),
                                                    {},
                                                    input.error()};
                        }
                        else
                        {
                            auto opened = SceneEditor::open(source, project, runtime, renderer, metadata, &*input,
                                                            binding, run_slot);
                            if (!opened)
                            {
                                failure = opened.error();
                            }
                            else
                            {
                                document = std::move(*opened);
                            }
                        }
                    }
                }
                if (failure.index() != 0)
                {
                    if (document)
                    {
                        document->requestClose();
                        document->poll(budget);
                        if (document->closeStatus().state != ECloseState::CLOSED)
                        {
                            return false;
                        }
                        document.reset();
                    }
                    if (binding)
                    {
                        binding->requestClose();
                        budget.render_programs -= binding->poll(budget.render_programs);
                        if (binding->state() != State::CLOSED)
                        {
                            return false;
                        }
                        binding.reset();
                    }
                    return lux::cxx::unexpected(std::move(std::get<EditorFailure>(failure)));
                }
                return document != nullptr;
            }

            EditorResult<std::unique_ptr<DocumentEditor>> adopt(Source &, Project &, process::ExecutionRuntime &)
            {
                return std::unique_ptr<DocumentEditor>(std::move(document));
            }
        };
    } // namespace

    DocumentRegistration sceneDocumentRegistration(process::ExecutionRuntime &runtime,
                                                   rendering::EditorRenderer &renderer, SceneEditorMetadata metadata)
    {
        return {
            std::string(kSceneDocumentType),
            [&runtime, &renderer, metadata = std::move(metadata), run_slot = std::make_shared<detail::SceneRunSlot>()](
                Project &project, const OpenDocumentRequest &request) -> EditorResult<std::unique_ptr<DocumentOpening>>
            {
                const auto *entry = project.asset(request.key.source);
                const bool valid =
                    request.key.project == project.manifest().id && request.key.type == kSceneDocumentType;
                if (!entry || entry->kind != EProjectAssetKind::SCENE || !runtime.blocking() || !metadata.scene ||
                    !metadata.render || !valid)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.open"});
                }
                return std::unique_ptr<DocumentOpening>(new lux::editor::detail::SourceOpening<SceneCodec>(
                    project, *entry, runtime, SceneCodec{renderer, metadata, run_slot}));
            }};
    }
} // namespace lux::editor::scene
