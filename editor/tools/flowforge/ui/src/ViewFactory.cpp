#include <lux/engine/editor/flowforge/FlowView.hpp>
namespace lux::editor::flowforge
{
    FlowViewResult<views::DetachedView> makeFlowView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        FlowViewServices services,
        std::optional<FlowViewBinding> binding,
        FlowViewState state
    )
    {
        auto created = FlowView::create(dispatcher, std::move(id), services, binding, std::move(state));
        if (!created)
            return cxx::unexpected(created.error());
        return views::DetachedView{
            contracts::CodeLease::builtin(),
            std::move(*created),
            +[](lux::ui::Pane& pane) -> views::ViewResult<void> {
                if (!static_cast<FlowView&>(pane).cancelEdit())
                    return cxx::unexpected(views::EViewError::BUSY);
                return {};
            }
        };
    }
}
