#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <lux/engine/editor/scene/RunInspectorView.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/ui/Root.hpp>

namespace lux::editor::scene
{
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
        {
            return cxx::unexpected(candidate.error());
        }
        auto bound = static_cast<SceneView*>(candidate->pane())->rebindRun(run);
        if (!bound)
        {
            return cxx::unexpected(bound.error());
        }
        return std::move(*candidate);
    }
    cxx::expected<std::shared_ptr<SceneInteractionGroup>, lux::ui::EAttachmentError> shareSceneInteraction(
        lux::ui::Root& root,
        lux::ui::PaneHandle source
    )
    {
        std::shared_ptr<SceneInteractionGroup> result;
        const auto read = [&](lux::ui::Pane& pane)
        {
            if (auto* view = dynamic_cast<SceneView*>(&pane))
            {
                result = view->interactionOwner();
            }
            else if (auto* view = dynamic_cast<OutlinerView*>(&pane))
            {
                result = view->interactionOwner();
            }
            else if (auto* view = dynamic_cast<InspectorView*>(&pane))
            {
                result = view->interactionOwner();
            }
            else if (auto* view = dynamic_cast<RunInspectorView*>(&pane))
            {
                result = view->interactionOwner();
            }
        };
        auto inspected = root.withPane(source, read);
        if (!inspected)
        {
            return cxx::unexpected(inspected.error());
        }
        if (!result)
        {
            return cxx::unexpected(lux::ui::EAttachmentError::INVALID_TREE);
        }
        return result;
    }
} // namespace lux::editor::scene
