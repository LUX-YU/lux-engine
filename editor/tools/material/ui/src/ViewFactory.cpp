#include <lux/engine/editor/material/MaterialView.hpp>
namespace lux::editor::material
{
    MaterialViewResult<views::DetachedView> makeMaterialView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        MaterialViewServices services,
        std::optional<MaterialViewBinding> binding,
        MaterialViewState state
    )
    {
        auto created = MaterialView::create(dispatcher, std::move(id), services, binding, state);
        if (!created)
            return cxx::unexpected(created.error());
        return views::DetachedView{
            contracts::CodeLease::builtin(),
            std::move(*created),
            +[](lux::ui::Pane& pane) -> views::ViewResult<void> {
                if (!static_cast<MaterialView&>(pane).cancelEdit())
                    return cxx::unexpected(views::EViewError::BUSY);
                return {};
            }
        };
    }
}
