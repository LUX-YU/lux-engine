#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/detail/EditorProject.hpp>

namespace lux::editor::detail
{
    EditorProject::EditorProject(std::unique_ptr<EditorContext> value) noexcept : context(std::move(value))
    {
        scenes = context->services().constructed<ProjectScenes>();
    }

    EditorProject::~EditorProject() noexcept = default;

    std::optional<exec::task<void>> EditorProject::close() noexcept
    {
        ui = {};
        scenes = context->services().constructed<ProjectScenes>();
        if (scenes)
        {
            scenes->closeAll();
            if (!scenes->settled())
            {
                return completeClose();
            }
        }
        context->close();
        return context->finishClose() ? std::nullopt : std::optional{completeClose()};
    }

    exec::task<void> EditorProject::completeClose() noexcept
    {
        if (scenes)
        {
            co_await scenes->closeAsync();
        }
        co_await context->closeAsync();
    }
} // namespace lux::editor::detail
