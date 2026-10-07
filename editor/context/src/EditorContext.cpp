#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/editor/detail/PreparedProject.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>

namespace lux::editor
{
    EditorContext::EditorContext(
        engine::EngineContext& engine,
        detail::PreparedProject prepared,
        EditorComposition&& composition
    ) noexcept
        : engine_(engine), project_(std::move(prepared.description)), manifest_(std::move(prepared.manifest)),
          plugins_(std::move(prepared.plugins)), registrations_(std::move(prepared.registrations)),
          ui_(std::move(composition.ui_)), scene_tools_(std::move(composition.scene_tools_)),
          scene_profiles_(std::move(composition.scene_profiles_)), services_(std::move(composition.services_)),
          tasks_(engine.execution())
    {
    }

    EditorContext::~EditorContext() noexcept = default;

    FrameworkResult<std::unique_ptr<EditorContext>> EditorContext::create(
        engine::EngineContext& engine,
        detail::PreparedProject prepared,
        EditorComposition&& composition
    ) noexcept
    {
        if (auto registered = registerProjectErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        if (auto registered = registerContextErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
        }
        if (auto valid = validateProjectManifest(prepared.manifest); !valid)
        {
            return cxx::unexpected(error::Error{
                Errors::ProjectManifest,
                {static_cast<std::uint64_t>(valid.error().code), valid.error().ordinal}
            });
        }
        const bool invalid_root = prepared.description.root.empty() || !prepared.description.root.is_absolute();
        const bool invalid_file = prepared.description.manifest_file.empty() ||
                                  !prepared.description.manifest_file.is_absolute() ||
                                  prepared.description.manifest_file.parent_path() != prepared.description.root;
        const bool invalid_summary =
            invalid_root || invalid_file || prepared.description.name != prepared.manifest.name;
        if (invalid_summary)
        {
            return cxx::unexpected(error::Error{Errors::EditorProjectNeedsANameAndAbsoluteRoot});
        }
        const bool missing_preparation = !prepared.plugins || !prepared.registrations;
        if (missing_preparation)
        {
            return cxx::unexpected(error::Error{Errors::ProjectNeedsPreparedPlugins});
        }
        return std::unique_ptr<EditorContext>(new EditorContext(engine, std::move(prepared), std::move(composition)));
    }

    process::TaskScope& EditorContext::tasks() noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            std::terminate();
        }
        return tasks_;
    }

    FrameworkResult<std::unique_ptr<EditorContext>> detail::createEditorContext(
        engine::EngineContext& engine,
        PreparedProject prepared,
        EditorComposition&& composition
    ) noexcept
    {
        return EditorContext::create(engine, std::move(prepared), std::move(composition));
    }
} // namespace lux::editor
