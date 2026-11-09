#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/detail/EditorProject.hpp>

namespace lux::editor::detail
{
    EditorProject::EditorProject(std::unique_ptr<EditorContext> value) noexcept : context(std::move(value)) {}

    EditorProject::~EditorProject() noexcept = default;

    exec::task<FrameworkResult<void>> EditorProject::close() noexcept
    {
        if (auto removed = ui.unmount(); !removed)
        {
            co_return removed;
        }
        co_return co_await context->closeAsync();
    }

    exec::task<FrameworkResult<void>> EditorProject::recoverClose() noexcept
    {
        // An earlier Root rejection may have left live Pane borrows. End those before recovering providers.
        if (auto removed = ui.unmount(); !removed)
        {
            co_return removed;
        }
        co_return co_await context->recoverClose();
    }
} // namespace lux::editor::detail
