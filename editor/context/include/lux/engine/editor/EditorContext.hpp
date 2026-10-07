#pragma once
#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/editor/EditorServiceRegistrar.hpp>
#include <lux/engine/editor/EditorUiRegistrar.hpp>
#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/editor/ProjectDescription.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/editor/SceneToolRegistrar.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

namespace lux::engine
{
    class EngineContext;
}
namespace lux::process
{
    class TaskScope;
}
namespace lux::project
{
    class PluginManager;
    struct SceneRegistrations;
} // namespace lux::project
namespace lux::editor
{
    namespace detail
    {
        struct PreparedProject;
    }
    class LuxEngine;
    class EditorContext final
    {
    public:
        using Assembly = cxx::function_ref<FrameworkResult<void>(EditorContext&) noexcept>;
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorContext>> create(
            engine::EngineContext&,
            ProjectDescription,
            ProjectManifest,
            Assembly
        ) noexcept;
        ~EditorContext() noexcept;
        EditorContext(const EditorContext&) = delete;
        EditorContext& operator=(const EditorContext&) = delete;
        EditorContext(EditorContext&&) = delete;
        EditorContext& operator=(EditorContext&&) = delete;

        [[nodiscard]] engine::EngineContext& engine() noexcept
        {
            return engine_;
        }
        [[nodiscard]] const engine::EngineContext& engine() const noexcept
        {
            return engine_;
        }
        [[nodiscard]] const ProjectDescription& project() const noexcept
        {
            return project_;
        }
        [[nodiscard]] asset::AssetVfs& assets() noexcept
        {
            return assets_;
        }
        [[nodiscard]] const asset::AssetVfs& assets() const noexcept
        {
            return assets_;
        }
        [[nodiscard]] EditorServiceRegistrar& services() noexcept
        {
            return services_;
        }
        [[nodiscard]] EditorUiRegistrar& ui() noexcept
        {
            return ui_;
        }
        [[nodiscard]] SceneToolRegistrar& sceneTools() noexcept
        {
            return scene_tools_;
        }

        [[nodiscard]] const ProjectManifest& manifest() const noexcept
        {
            return manifest_;
        }
        [[nodiscard]] const project::PluginManager* plugins() const noexcept
        {
            return plugins_.get();
        }
        [[nodiscard]] const project::SceneRegistrations* sceneRegistrations() const noexcept
        {
            return registrations_.get();
        }
        [[nodiscard]] process::TaskScope& tasks() noexcept;
        void beginClose() noexcept;
        [[nodiscard]] bool closed() const noexcept;
        [[nodiscard]] bool closing() const noexcept
        {
            return closing_;
        }

        template <class T> [[nodiscard]] FrameworkResult<std::reference_wrapper<T>> service() noexcept
        {
            return services_.get<T>(*this);
        }

    private:
        friend class LuxEngine;
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorContext>> create(
            engine::EngineContext&,
            detail::PreparedProject,
            Assembly
        ) noexcept;
        EditorContext(engine::EngineContext&, detail::PreparedProject);
        bool closing_{};
        bool assembled_{};
        void freeze() noexcept;
        engine::EngineContext& engine_;
        ProjectDescription project_;
        ProjectManifest manifest_;
        std::unique_ptr<project::PluginManager> plugins_;
        std::shared_ptr<const project::SceneRegistrations> registrations_;
        asset::AssetVfs assets_;
        EditorUiRegistrar ui_;
        SceneToolRegistrar scene_tools_;
        // Services die before factory captures, project paths and VFS providers.
        EditorServiceRegistrar services_;
        // Transport callbacks settle while services, VFS and plugin code are still alive.
        std::unique_ptr<process::TaskScope> tasks_;
    };
} // namespace lux::editor
