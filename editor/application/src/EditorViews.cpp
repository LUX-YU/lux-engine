#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
namespace lux::editor::application
{
    EditorResult<lux::ui::PaneHandle> EditorApplication::Impl::adopt(
        std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>& candidate
    )
    {
        auto* pane = candidate.get();
        auto mounted = desktop_->root().addSubPane(std::move(candidate));
        if (!mounted)
        {
            return applicationFailure("view.mount", mounted.error());
        }
        auto id = desktop_->root().identify(*pane);
        if (!id)
        {
            return applicationFailure("view.identity", id.error());
        }
        return *id;
    }
} // namespace lux::editor::application
