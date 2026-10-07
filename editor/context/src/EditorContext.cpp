#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/detail/PreparedProject.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/process/TaskScope.hpp>

namespace lux::editor
{
    EditorContext::EditorContext(engine::EngineContext& engine, detail::PreparedProject prepared)
        : engine_(engine), project_(std::move(prepared.description)), manifest_(std::move(prepared.manifest)),
          plugins_(std::move(prepared.plugins)), registrations_(std::move(prepared.registrations)),
          tasks_(std::make_unique<process::TaskScope>(engine.execution()))
    {
    }
    EditorContext::~EditorContext() noexcept
    {
        beginClose();
        // The host reaches this only after settled(). Direct RAII users must keep the
        // original transport-only completion contract during TaskScope destruction.
    }
    FrameworkResult<std::unique_ptr<EditorContext>> EditorContext::create(
        engine::EngineContext& engine,
        ProjectDescription description,
        ProjectManifest manifest,
        Assembly assembly
    ) noexcept
    {
        if (auto registered = registerFrameworkErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        if (!manifest.plugins.empty())
        {
            return cxx::unexpected(error::Error{Errors::ProjectNeedsPreparedPlugins});
        }
        auto plugins = project::PluginManager::create({}, {});
        if (!plugins)
        {
            return cxx::unexpected(
                error::Error{Errors::ProjectPlugins, {static_cast<std::uint64_t>(plugins.error().code)}}
            );
        }
        return create(
            engine,
            {std::move(description),
             std::move(manifest),
             std::make_unique<project::PluginManager>(std::move(*plugins)),
             {}},
            assembly
        );
    }
    FrameworkResult<std::unique_ptr<EditorContext>> EditorContext::create(
        engine::EngineContext& engine,
        detail::PreparedProject prepared,
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
        if (auto valid = validateProjectManifest(prepared.manifest); !valid)
        {
            return cxx::unexpected(error::Error{
                Errors::ProjectManifest,
                {static_cast<std::uint64_t>(valid.error().code), valid.error().ordinal}
            });
        }
        const bool invalid_root = prepared.description.root.empty() || !prepared.description.root.is_absolute();
        if (invalid_root || prepared.description.name != prepared.manifest.name)
        {
            return cxx::unexpected(error::Error{Errors::EditorProjectNeedsANameAndAbsoluteRoot});
        }
        auto result = std::unique_ptr<EditorContext>(new EditorContext(engine, std::move(prepared)));
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
        scene_profiles_.freeze();
        assembled_ = true;
    }
    process::TaskScope& EditorContext::tasks() noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent() || !assembled_)
        {
            std::terminate();
        }
        return *tasks_;
    }
    void EditorContext::beginClose() noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            std::terminate();
        }
        if (!closing_)
        {
            closing_ = true;
            tasks_->requestStop();
        }
    }
    bool EditorContext::closed() const noexcept
    {
        return closing_ && tasks_->settled();
    }
} // namespace lux::editor
