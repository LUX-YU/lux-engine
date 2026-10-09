#pragma once
#include <lux/engine/ContextExtensions.hpp>
#include <lux/engine/editor/EditorServices.hpp>
#include <lux/engine/editor/EditorUiRegistry.hpp>
#include <lux/engine/editor/ProjectDescription.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/editor/SceneProfileRegistry.hpp>
#include <lux/engine/editor/SceneToolRegistry.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

namespace lux::engine
{
    class EngineContext;
}
namespace lux::project
{
    class PluginManager;
    struct SceneRegistrations;
} // namespace lux::project
namespace lux::editor
{
    class EditorContext;
    class EditorComposition;
    namespace detail
    {
        struct PreparedProject;
        [[nodiscard]] FrameworkResult<std::unique_ptr<EditorContext>>
        createEditorContext(engine::EngineContext&, PreparedProject, EditorComposition&&) noexcept;
    } // namespace detail
    class EditorContext final
    {
    public:
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
        [[nodiscard]] const ProjectManifest& manifest() const noexcept
        {
            return manifest_;
        }
        [[nodiscard]] const project::PluginManager& plugins() const noexcept
        {
            return *plugins_;
        }
        [[nodiscard]] const project::SceneRegistrations& sceneRegistrations() const noexcept
        {
            return *registrations_;
        }
        [[nodiscard]] asset::AssetVfs& assets() noexcept
        {
            return assets_;
        }
        [[nodiscard]] const asset::AssetVfs& assets() const noexcept
        {
            return assets_;
        }
        [[nodiscard]] EditorServices& services() noexcept
        {
            return services_;
        }
        [[nodiscard]] const EditorUiRegistry& ui() const noexcept
        {
            return ui_;
        }
        [[nodiscard]] const SceneToolRegistry& sceneTools() const noexcept
        {
            return scene_tools_;
        }
        [[nodiscard]] const SceneProfileRegistry& sceneProfiles() const noexcept
        {
            return scene_profiles_;
        }
        [[nodiscard]] engine::ContextExtensions& extensions() noexcept
        {
            return extensions_;
        }

        [[nodiscard]] const engine::ContextExtensions& extensions() const noexcept
        {
            return extensions_;
        }

        [[nodiscard]] process::TaskScope& tasks() noexcept;
        template <class T> [[nodiscard]] FrameworkResult<std::reference_wrapper<T>> service() noexcept
        {
            return services_.get<T>(*this);
        }

    private:
        friend FrameworkResult<std::unique_ptr<EditorContext>> detail::
            createEditorContext(engine::EngineContext&, detail::PreparedProject, EditorComposition&&) noexcept;
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorContext>>
        create(engine::EngineContext&, detail::PreparedProject, EditorComposition&&) noexcept;
        EditorContext(engine::EngineContext&, detail::PreparedProject, EditorComposition&&) noexcept;
        engine::EngineContext& engine_;
        ProjectDescription project_;
        ProjectManifest manifest_;
        std::unique_ptr<project::PluginManager> plugins_;
        std::shared_ptr<const project::SceneRegistrations> registrations_;
        asset::AssetVfs assets_;
        EditorUiRegistry ui_;
        SceneToolRegistry scene_tools_;
        SceneProfileRegistry scene_profiles_;
        engine::ContextExtensions extensions_;
        // Instances die before factory captures, project paths, VFS and plugin registrations.
        EditorServices services_;
        // Destruction requests cancellation only; accepted tasks own their inputs and code.
        process::TaskScope tasks_;
    };
} // namespace lux::editor
