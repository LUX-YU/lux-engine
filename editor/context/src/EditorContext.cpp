#include <lux/engine/editor/EditorContext.hpp>

namespace lux::editor
{
    EditorContext::EditorContext(
        engine::EngineContext& engine,
        ProjectDescription project
    )
        : engine_(engine), project_(std::move(project))
    {
    }
    void EditorContext::freeze() noexcept
    {
        services_.freeze();
        ui_.freeze();
        scene_tools_.freeze();
    }
    FrameworkResult<void> validateProject(const ProjectDescription& project) noexcept
    {
        const bool is_invalid_name = project.name.empty();
        const bool is_invalid_root = project.root.empty() || !project.root.is_absolute();
        if (is_invalid_name || is_invalid_root)
        {
            return cxx::unexpected(
                FrameworkFailure{EFrameworkError::INVALID_DESCRIPTION, "Project needs a name and absolute root"}
            );
        }
        return {};
    }
} // namespace lux::editor
