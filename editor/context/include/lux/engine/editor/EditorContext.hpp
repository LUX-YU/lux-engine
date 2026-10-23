#pragma once
#include <lux/engine/ContextExtensions.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/editor/EditorServices.hpp>
#include <lux/engine/editor/EditorUiRegistry.hpp>
#include <lux/engine/editor/ProjectDescription.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/editor/SceneProfileRegistry.hpp>
#include <lux/engine/editor/SceneToolRegistry.hpp>
#include <lux/engine/process/asset_loading/AssetRead.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/scene/SceneInstanceLease.hpp>

namespace lux::process::asset_loading
{
    class VfsAssetReadEndpoint;
}

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

        // Main adopts a scene catalog already published by the project's single file-writing operation.
        // Plugin selection and project identity remain fixed for this Context's lifetime.
        [[nodiscard]] ProjectResult<void> adoptSceneCatalog(
            const ProjectManifest& expected,
            std::vector<ProjectManifest::SceneRecord>,
            std::optional<asset::AssetId> startup
        ) noexcept;

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

        [[nodiscard]] execution::TaskScope& tasks() noexcept;
        // Main-only lazy project VFS endpoint. IO uses the Engine's bounded blocking resource.
        // Copied ports close with this Context; accepted reads retain their source snapshot and code.
        [[nodiscard]] FrameworkResult<process::asset_loading::AssetReadPort> prepareAssetReads() noexcept;
        // Adopt a Scene built with this project's service borrows. The existing SceneRuntime owns
        // its systems; this Context owns only the retirement responsibility. No second Scene table.
        [[nodiscard]] FrameworkResult<scene::SceneInstanceId> adoptScene(scene::SceneInstanceLease&&) noexcept;
        // Revoke this Context's adopted instance, retaining the receipt through project close.
        [[nodiscard]] FrameworkResult<scene::InstanceRetirement> retireScene(scene::SceneInstanceId) noexcept;
        // Main-only, nonblocking: revoke Scene production first. Continue SceneRuntime driving and
        // maintenance frames, then call finishClose until providers can be destroyed.
        void close() noexcept;
        [[nodiscard]] bool finishClose() noexcept;

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
        execution::TaskScope tasks_;
        std::unique_ptr<process::asset_loading::VfsAssetReadEndpoint> asset_reads_;
        std::vector<scene::SceneInstanceLease> scenes_;
        std::vector<scene::InstanceRetirement> scene_retirements_;
        enum class ECloseStage : std::uint8_t
        {
            OPEN,
            SCENES,
            TASKS
        };
        ECloseStage close_stage_ = ECloseStage::OPEN;
    };
} // namespace lux::editor
