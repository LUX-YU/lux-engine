#include <lux/engine/editor/scene/SceneView.hpp>
namespace lux::editor::scene
{
    SceneViewResult<views::DetachedView> makeSceneView(
        object::ObjectDispatcherRef dispatcher,
        SceneViewServices services,
        SceneViewCreateInfo info
    )
    {
        auto view = SceneView::create(dispatcher, services, std::move(info));
        if (!view)
            return cxx::unexpected(view.error());
        return views::DetachedView{
            contracts::CodeLease::builtin(),
            std::move(*view),
            +[](lux::ui::Pane& pane) -> views::ViewResult<void> {
                auto& scene = static_cast<SceneView&>(pane);
                if (std::holds_alternative<UnboundSceneBinding>(scene.binding()))
                    return {};
                if (!scene.cancelEdit())
                    return cxx::unexpected(views::EViewError::BUSY);
                return {};
            }
        };
    }
}
