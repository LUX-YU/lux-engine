#include <lux/engine/editor/gui/scene/SceneDocumentProvider.hpp>
#include <lux/engine/editor/gui/GuiFrontend.hpp>
#include <lux/engine/editor/gui/scene/ScenePane.hpp>
#include <lux/engine/editor/gui/scene/OutlinerPane.hpp>
#include <lux/engine/editor/gui/scene/InspectorPane.hpp>
#include <lux/engine/editor/gui/scene/ResourcePane.hpp>
#include <lux/engine/editor/ui/SceneLayout.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/simulation/TransformSystem.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <lux/engine/function/render/client/core/RenderFeatureMetaModule.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <algorithm>

namespace lux::editor::gui
{
    namespace
    {
        EditorResult<std::shared_ptr<const lux::scene::SceneMetaManager>> sceneMetadata(
            std::span<const lux::simulation::ecs::ComponentSchema> additional)
        {
            lux::meta::ReflectionRegistry::drainPending();
            lux::scene::initializeBuiltinRenderSystemMeta();
            lux::render::initializeBuiltinRenderFeatureMeta();
            std::vector<lux::simulation::ecs::ComponentSchema> schemas;
            const auto append = [&schemas](auto values)
            { schemas.insert(schemas.end(), values.begin(), values.end()); };
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
            auto metadata = lux::scene::SceneMetaManager::build({std::move(*set),
                                                                 std::move(systems),
                                                                 {render.begin(), render.end()},
                                                                 {features.begin(), features.end()},
                                                                 {bindings.begin(), bindings.end()}});
            if (!metadata)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.metadata",
                                                          static_cast<std::uint64_t>(metadata.error().code)});
            }
            return std::make_shared<const lux::scene::SceneMetaManager>(std::move(*metadata));
        }

        struct PaneAttachment final
        {
            std::vector<std::unique_ptr<DocumentView>> views;

            ~PaneAttachment()
            {
                // These panes have not been adopted/polled: no RenderView or asynchronous work exists yet.
                PollBudget budget;
                for (auto &view : views)
                {
                    view->requestClose();
                    view->poll(budget);
                }
            }
        };

        template <class Pane, class... Arguments>
        EditorResult<void> attachPane(PaneAttachment &batch, scene::SceneEditor &document, EditorWindow &window,
                                      Arguments &&...args)
        {
            auto pane = std::make_unique<Pane>(document, std::forward<Arguments>(args)...);
            auto attached = pane->attach(window.uiSession());
            if (!attached)
            {
                pane->requestClose();
                return attached;
            }
            batch.views.push_back(std::move(pane));
            return {};
        }
    } // namespace

    GuiDocumentProvider sceneDocumentProvider(std::span<const lux::simulation::ecs::ComponentSchema> components,
                                               std::span<const ComponentBinding> additional_bindings)
    {
        auto bindings = firstPartyComponentBindings();
        bindings.insert(bindings.end(), additional_bindings.begin(), additional_bindings.end());
        std::ranges::sort(bindings, {}, [](const ComponentBinding& value) { return value.type.hash(); });
        auto shared_bindings = std::make_shared<const std::vector<ComponentBinding>>(std::move(bindings));

        return {
            std::string(scene::kSceneDocumentType),
            [](const ProjectAssetEntry &asset) { return asset.kind == EProjectAssetKind::SCENE; },
            [schemas = std::vector(components.begin(), components.end()), shared_bindings]
            (Editor &editor, process::ExecutionRuntime &runtime,
               rendering::EditorRenderer &renderer) -> EditorResult<void>
            {
                auto metadata = sceneMetadata(schemas);
                if (!metadata)
                {
                    return lux::cxx::unexpected(metadata.error());
                }
                std::uint64_t previous{};
                for (const auto& binding : *shared_bindings)
                {
                    const auto* schema = (*metadata)->getComponentMeta(binding.type);
                    const bool invalid = !schema || !schema->editor_visible || !binding.draw || binding.name.empty();
                    if (invalid || previous == binding.type.hash())
                    {
                        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "inspector.binding",
                            binding.type.hash(), "Inspector bindings require unique visible component schemas"});
                    }
                    previous = binding.type.hash();
                }
                return editor.registerDocument(
                    {std::string(scene::kSceneDocumentType), [&runtime, &renderer, metadata = std::move(*metadata)](
                                                                 Project &project, const OpenDocumentRequest &request)
                     { return scene::openSceneDocument(project, request, runtime, renderer, metadata); }});
            },
            [shared_bindings](DocumentEditor &base, EditorWindow &window, rendering::EditorRenderer &renderer, process::ExecutionRuntime& runtime) -> EditorResult<void>
            {
                auto *document = dynamic_cast<scene::SceneEditor *>(&base);
                if (!document)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.gui"});
                }
                const auto prefix = "scene-" + std::to_string(document->historyId().value);
                PaneAttachment batch;
                batch.views.reserve(4);
                auto outliner = attachPane<OutlinerPane>(batch, *document, window, prefix + "-outliner");
                if (!outliner)
                {
                    return outliner;
                }
                auto inspector = attachPane<InspectorPane>(batch, *document, window, prefix + "-inspector", shared_bindings);
                if (!inspector)
                {
                    return inspector;
                }
                auto resources = attachPane<ResourcePane>(batch, *document, window, window, runtime, prefix + "-resources");
                if (!resources)
                {
                    return resources;
                }
                auto scene = attachPane<ScenePane>(batch, *document, window, renderer, prefix + "-view");
                if (!scene)
                {
                    return scene;
                }
                const auto layout = window.installLayout(ui::sceneLayout(prefix));
                if (!layout)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "scene.layout",
                                                              static_cast<std::uint64_t>(layout.error().code)});
                }
                return document->addViews(batch.views);
            }};
    }
} // namespace lux::editor::gui
