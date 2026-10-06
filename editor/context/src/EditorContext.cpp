#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>

namespace lux::editor
{
    EditorContext::EditorContext(engine::EngineContext& engine, ProjectDescription project)
        : engine_(engine), project_(std::move(project))
    {
    }
    FrameworkResult<std::unique_ptr<EditorContext>> EditorContext::create(
        engine::EngineContext& engine,
        ProjectDescription project,
        Assembly assembly
    ) noexcept
    {
        if (auto registered = registerFrameworkErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
        }
        if (auto valid = validateProject(project); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        auto result = std::unique_ptr<EditorContext>(new EditorContext(engine, std::move(project)));
        if (auto assembled = assembly(*result); !assembled)
        {
            return cxx::unexpected(assembled.error());
        }
        result->freeze();
        return result;
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
            return cxx::unexpected(error::Error{Errors::EditorProjectNeedsANameAndAbsoluteRoot, {}});
        }
        return {};
    }
} // namespace lux::editor
