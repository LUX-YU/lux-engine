#pragma once
#include <exec/task.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/editor/EditorFacilities.hpp>
#include <lux/engine/editor/EditorServices.hpp>
#include <lux/engine/editor/EditorUiRegistry.hpp>
#include <lux/engine/editor/ProjectDescription.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/editor/SceneProfileRegistry.hpp>
#include <lux/engine/editor/ValueEditors.hpp>
#include <lux/engine/process/assets/AssetRepository.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>

namespace lux::process::assets
{
    class AssetRepository;
}

namespace lux::engine
{
    class Engine;
}

namespace lux::plugins
{
    class PluginManager;
    struct SceneRegistrations;
} // namespace lux::plugins

namespace lux::editor
{
    class EditorContext;
    class EditorComposition;

    namespace detail
    {
        struct PreparedProject;
        [[nodiscard]] FrameworkResult<std::unique_ptr<EditorContext>>
        createEditorContext(engine::Engine&, PreparedProject, EditorComposition&&) noexcept;
    } // namespace detail

    class EditorContext final : public object::LuxObject
    {
    public:
        // A fully initialized Scene has entered this project's membership. Engine Scene creation
        // alone does not establish that relationship and must not drive project-scoped subscriptions.
        object::TSignal<scene::SceneInstanceId> sceneAdded{*this};

        ~EditorContext() noexcept;
        EditorContext(const EditorContext&) = delete;
        EditorContext& operator=(const EditorContext&) = delete;
        EditorContext(EditorContext&&) = delete;
        EditorContext& operator=(EditorContext&&) = delete;

        [[nodiscard]] engine::Engine& engine() noexcept
        {
            return engine_;
        }

        [[nodiscard]] const engine::Engine& engine() const noexcept
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

        [[nodiscard]] const plugins::PluginManager& plugins() const noexcept
        {
            return *plugins_;
        }

        [[nodiscard]] const plugins::SceneRegistrations& sceneRegistrations() const noexcept
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

        [[nodiscard]] const SceneProfileRegistry& sceneProfiles() const noexcept
        {
            return scene_profiles_;
        }

        [[nodiscard]] EditorFacilities& extensions() noexcept
        {
            return extensions_;
        }

        [[nodiscard]] const EditorFacilities& extensions() const noexcept
        {
            return extensions_;
        }

        [[nodiscard]] execution::TaskScope& tasks() noexcept;
        // Immutable function declarations; returned borrows remain valid through this Context's pane lifetime.
        [[nodiscard]] const ComponentEditor* componentEditor(cxx::type_info) const noexcept;
        [[nodiscard]] const SceneConfigurationEditor* sceneConfigurationEditor(cxx::type_info) const noexcept;
        // Construct after project mounts are published. Consumers borrow this fixed-source repository.
        [[nodiscard]] FrameworkResult<process::assets::AssetRepository*> prepareAssets() noexcept;
        // Create and associate at one uninterrupted publication point. The Context
        // records membership IDs only; Runtime uniquely owns instances and close operations.
        [[nodiscard]] FrameworkResult<scene::SceneInstanceId> createScene(scene::SceneRuntime::Builder&) noexcept;
        [[nodiscard]] exec::task<FrameworkResult<scene::SceneInstanceId>>
        createSceneAsync(scene::SceneRuntime::Builder&) noexcept;

        [[nodiscard]] std::span<const scene::SceneInstanceId> scenes() const noexcept
        {
            return scenes_;
        }

        // Revoke this Context's adopted instance, retaining the receipt through project close.
        [[nodiscard]] FrameworkResult<scene::SceneCloseReceipt> retireScene(scene::SceneInstanceId) noexcept;
        // Main-only, nonblocking: revoke Scene production first. Continue SceneRuntime driving and
        // maintenance frames, then call finishClose until providers can be destroyed.
        void close() noexcept;
        [[nodiscard]] bool finishClose() noexcept;
        // Await outside this Context's TaskScope. The caller retains the Context until completion.
        [[nodiscard]] exec::task<FrameworkResult<void>> closeAsync() noexcept;
        [[nodiscard]] exec::task<FrameworkResult<void>> recoverClose() noexcept;

        template <class T> [[nodiscard]] FrameworkResult<std::reference_wrapper<T>> service() noexcept
        {
            return services_.get<T>(*this);
        }

    private:
        friend FrameworkResult<std::unique_ptr<EditorContext>>
        detail::createEditorContext(engine::Engine&, detail::PreparedProject, EditorComposition&&) noexcept;
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorContext>>
        create(engine::Engine&, detail::PreparedProject, EditorComposition&&) noexcept;
        EditorContext(engine::Engine&, detail::PreparedProject, EditorComposition&&) noexcept;
        [[nodiscard]] FrameworkResult<void> retireScenes() noexcept;
        engine::Engine& engine_;
        ProjectDescription project_;
        ProjectManifest manifest_;
        std::unique_ptr<plugins::PluginManager> plugins_;
        std::shared_ptr<const plugins::SceneRegistrations> registrations_;
        asset::AssetVfs assets_;
        EditorUiRegistry ui_;
        SceneProfileRegistry scene_profiles_;
        std::vector<ComponentEditor> component_editors_;
        std::vector<SceneConfigurationEditor> scene_configuration_editors_;
        EditorFacilities extensions_;
        // Instances die before factory captures, project paths, VFS and plugin registrations.
        std::unique_ptr<process::assets::AssetRepository> loaded_assets_;
        EditorServices services_;
        // Destruction requests cancellation only; accepted tasks own their inputs and code.
        execution::TaskScope tasks_;
        std::vector<scene::SceneInstanceId> scenes_;
        std::vector<scene::SceneCloseReceipt> scene_retirements_;
        enum class ECloseStage : std::uint8_t
        {
            OPEN,
            SERVICES,
            SCENES,
            TASKS
        };
        ECloseStage close_stage_ = ECloseStage::OPEN;
    };
} // namespace lux::editor
