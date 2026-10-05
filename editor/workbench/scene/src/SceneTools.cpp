#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <lux/engine/editor/scene/RunInspectorView.hpp>
#include <lux/engine/editor/scene/ResourceView.hpp>
#include <lux/engine/editor/workbench/ViewPreparation.hpp>
#include <lux/engine/ui/Root.hpp>

namespace lux::editor::scene
{
    namespace
    {
        template <class Error> auto failure(const Error& error)
        {
            const auto detail = workbench::detail::viewPreparationFailure(
                error, workbench::detail::isRetryableViewFailure(error)
            );
            return cxx::unexpected(views::ViewFactoryFailure{
                detail.retryable ? views::EViewFactoryError::BUSY : views::EViewFactoryError::CONSTRUCT,
                detail.domain, detail.code, detail.message
            });
        }
        auto failure(const SceneConfigurationFailure& error)
        {
            return cxx::unexpected(views::ViewFactoryFailure{
                error.code == ESceneConfigurationError::BUSY ? views::EViewFactoryError::BUSY
                                                             : views::EViewFactoryError::CONSTRUCT,
                error.domain, error.reason, error.message
            });
        }
        template <class Result> views::ViewFactoryResult<views::DetachedView> toolResult(Result result)
        {
            if (!result)
                return failure(result.error());
            return std::move(*result);
        }
    }
    SceneViewResult<views::DetachedView> makeRunSceneView(
        object::ObjectDispatcherRef dispatcher,
        SceneViewServices services,
        lux::ui::PaneId id,
        RunId run,
        system::SystemInstanceId render_system
    )
    {
        SceneViewCreateInfo input;
        input.id = std::move(id);
        input.title = "Run (frozen author content)";
        input.state.camera.transform.translation = {0, 3, 8};
        input.render_system = render_system;
        auto candidate = makeSceneView(dispatcher, services, std::move(input));
        if (!candidate)
            return cxx::unexpected(candidate.error());
        auto bound = static_cast<SceneView*>(candidate->pane())->rebindRun(run);
        if (!bound)
            return cxx::unexpected(bound.error());
        return std::move(*candidate);
    }
    cxx::expected<std::shared_ptr<SceneInteractionGroup>, lux::ui::EAttachmentError>
    shareSceneInteraction(
        lux::ui::Root& root, lux::ui::PaneHandle source
    )
    {
        std::shared_ptr<SceneInteractionGroup> result;
        const auto read = [&](lux::ui::Pane& pane) {
            if (auto* view = dynamic_cast<SceneView*>(&pane))
                result = view->interactionOwner();
            else if (auto* view = dynamic_cast<OutlinerView*>(&pane))
                result = view->interactionOwner();
            else if (auto* view = dynamic_cast<InspectorView*>(&pane))
                result = view->interactionOwner();
            else if (auto* view = dynamic_cast<RunInspectorView*>(&pane))
                result = view->interactionOwner();
        };
        auto inspected = root.withPane(source, read);
        if (!inspected)
            return cxx::unexpected(inspected.error());
        if (!result)
            return cxx::unexpected(lux::ui::EAttachmentError::INVALID_TREE);
        return result;
    }
    views::ViewFactoryResult<views::DetachedView> makeSceneToolView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        lux::ui::Root& root,
        lux::ui::PaneHandle source,
        ESceneTool tool,
        SceneToolInputs inputs
    )
    {
        auto shared = shareSceneInteraction(root, source);
        if (!shared)
            return failure(shared.error());
        auto group = std::move(*shared);
        const auto run = group->run();
        const auto author = group->session();
        if (!run && !author)
            return failure(views::EViewError::INVALID_ID);
        const VSceneViewBinding binding = run
            ? VSceneViewBinding{RunningSceneBinding{*run, group.get()}}
            : VSceneViewBinding{EditedSceneBinding{*author, group.get()}};
        switch (tool)
        {
        case ESceneTool::OUTLINER:
            return toolResult(makeOutlinerView(
                dispatcher, std::move(id), inputs.scene.sessions, binding, inputs.scene.runs,
                inputs.schemas, std::move(group)
            ));
        case ESceneTool::INSPECTOR:
        {
            const auto& selected = group->selection().objects;
            if (selected.empty())
                return failure(views::EViewError::INVALID_ID);
            if (run)
            {
                const auto* target = std::get_if<RunningObjectRef>(&selected.front());
                if (!target)
                    return failure(views::EViewError::INVALID_ID);
                auto result = makeRunInspectorView(
                    dispatcher, std::move(id), inputs.runs, *target, inputs.schemas,
                    runInspectorComponents(), inputs.assets, std::move(group)
                );
                if (!result)
                    return failure(result.error());
                return std::move(*result);
            }
            const auto* target = std::get_if<SceneObjectRef>(&selected.front());
            if (!target)
                return failure(views::EViewError::INVALID_ID);
            return toolResult(makeInspectorView(
                dispatcher, std::move(id), inputs.scene.sessions, std::get<EditedSceneBinding>(binding),
                *target, inputs.schemas, std::move(inputs.components), inputs.assets, std::move(group)
            ));
        }
        case ESceneTool::RESOURCES:
        {
            auto result = makeResourceView(dispatcher, std::move(id), inputs.scene.runtime);
            if (!result)
                return failure(result.error());
            auto followed = static_cast<ResourceView*>(result->pane())->followViewport(root, source);
            if (!followed)
                return failure(followed.error());
            return std::move(*result);
        }
        case ESceneTool::CONFIGURATION:
        {
            if (!author)
                return failure(views::EViewError::INVALID_ID);
            auto result = makeSceneConfigurationView(
                dispatcher, std::move(id), inputs.scene.sessions, std::move(inputs.configuration), *author
            );
            if (!result)
                return failure(result.error());
            return std::move(*result);
        }
        }
        return failure(views::EViewError::INVALID_ID);
    }
}
