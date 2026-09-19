#include <algorithm>
#include <lux/engine/editor/gui/GuiFrontend.hpp>
#include <lux/engine/editor/gui/scene/InspectorPane.hpp>
#include <lux/engine/editor/gui/scene/OutlinerPane.hpp>
#include <lux/engine/editor/gui/scene/ResourcePane.hpp>
#include <lux/engine/editor/gui/scene/SceneDocumentProvider.hpp>
#include <lux/engine/editor/gui/scene/ScenePane.hpp>
#include <lux/engine/editor/ui/SceneLayout.hpp>

namespace lux::editor::gui
{
    namespace
    {
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
                                      std::string_view id, Arguments &&...args)
        {
            const auto existing =
                std::ranges::find_if(document.views(), [id](const auto &view) { return view->id() == id; });
            if (existing != document.views().end())
            {
                if ((*existing)->closeStatus().state != ECloseState::OPEN)
                {
                    return lux::cxx::unexpected(
                        EditorFailure{EEditorError::BUSY, "scene.views", 0, "The previous pane is still closing"});
                }
                return {};
            }
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
        std::ranges::sort(bindings, {}, [](const ComponentBinding &value) { return value.type.hash(); });
        auto shared_bindings = std::make_shared<const std::vector<ComponentBinding>>(std::move(bindings));

        return {std::string(scene::kSceneDocumentType),
                [](const ProjectAssetEntry &asset) { return asset.kind == EProjectAssetKind::SCENE; },
                [schemas = std::vector(components.begin(), components.end()),
                 shared_bindings](Editor &editor, process::ExecutionRuntime &runtime,
                                  rendering::EditorRenderer &renderer) -> EditorResult<void>
                {
                    auto metadata = scene::sceneMetadata(schemas);
                    if (!metadata)
                    {
                        return lux::cxx::unexpected(metadata.error());
                    }
                    std::uint64_t previous{};
                    for (const auto &binding : *shared_bindings)
                    {
                        const auto *schema = metadata->scene->getComponentMeta(binding.type);
                        const bool invalid =
                            !schema || !schema->editor_visible || !binding.draw || binding.name.empty();
                        if (invalid || previous == binding.type.hash())
                        {
                            return lux::cxx::unexpected(
                                EditorFailure{EEditorError::INVALID_ARGUMENT, "inspector.binding", binding.type.hash(),
                                              "Inspector bindings require unique visible component schemas"});
                        }
                        previous = binding.type.hash();
                    }
                    return editor.registerDocument(
                        scene::sceneDocumentRegistration(runtime, renderer, std::move(*metadata)));
                },
                [shared_bindings](DocumentEditor &base, EditorWindow &window, rendering::EditorRenderer &renderer,
                                  process::ExecutionRuntime &runtime) -> EditorResult<void>
                {
                    auto *document = dynamic_cast<scene::SceneEditor *>(&base);
                    if (!document)
                    {
                        return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.gui"});
                    }
                    const auto prefix = "scene-" + std::to_string(document->handle().index) + "-" +
                                        std::to_string(document->handle().gen);
                    PaneAttachment batch;
                    batch.views.reserve(4);
                    auto outliner =
                        attachPane<OutlinerPane>(batch, *document, window, prefix + "-outliner", prefix + "-outliner");
                    if (!outliner)
                    {
                        return outliner;
                    }
                    auto inspector = attachPane<InspectorPane>(batch, *document, window, prefix + "-inspector",
                                                               prefix + "-inspector", shared_bindings);
                    if (!inspector)
                    {
                        return inspector;
                    }
                    auto resources = attachPane<ResourcePane>(batch, *document, window, prefix + "-resources", window,
                                                              runtime, prefix + "-resources");
                    if (!resources)
                    {
                        return resources;
                    }
                    auto scene =
                        attachPane<ScenePane>(batch, *document, window, prefix + "-view", renderer, prefix + "-view");
                    if (!scene)
                    {
                        return scene;
                    }
                    if (batch.views.empty())
                    {
                        return {};
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
