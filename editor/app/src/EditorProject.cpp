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

    void EditorProject::beginClose() noexcept
    {
        ui = {};
        scenes = context->services().constructed<ProjectScenes>();
        if (scenes)
        {
            scenes->closeAll();
        }
    }

    bool EditorProject::advanceClose() noexcept
    {
        if (scenes)
        {
            scenes->maintain();
            if (!scenes->settled())
            {
                return false;
            }
        }
        context->close();
        return context->finishClose();
    }
} // namespace lux::editor::detail
