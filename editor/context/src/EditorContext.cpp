#include <algorithm>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/editor/detail/PreparedProject.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>

namespace lux::editor
{
    EditorContext::EditorContext(
        engine::EngineContext& engine,
        detail::PreparedProject prepared,
        EditorComposition&& composition
    ) noexcept
        : engine_(engine), project_(std::move(prepared.description)), manifest_(std::move(prepared.manifest)),
          plugins_(std::move(prepared.plugins)), registrations_(std::move(prepared.registrations)),
          ui_(std::move(composition.ui_)), scene_profiles_(std::move(composition.scene_profiles_)),
          extensions_(std::move(composition.extensions_).publish()), services_(std::move(composition.services_)),
          tasks_(engine.execution())
    {
    }

    EditorContext::~EditorContext() noexcept
    {
        // Contexts without Scene borrows retain the existing nonblocking task-destruction contract.
        if (!scenes_.empty() || (!scene_retirements_.empty() && !finishClose()))
        {
            std::terminate();
        }
    }

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

    execution::TaskScope& EditorContext::tasks() noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            std::terminate();
        }
        return tasks_;
    }

    FrameworkResult<process::asset_loading::AssetReadPort> EditorContext::prepareAssetReads() noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
        }
        if (!asset_reads_)
        {
            auto blocking = engine_.execution().blocking();
            if (!blocking)
            {
                return cxx::unexpected(execution::toError(blocking.error()));
            }
            auto endpoint = process::asset_loading::VfsAssetReadEndpoint::create(
                assets_.view(),
                *blocking,
                (tasks_).execution(),
                {16},
                (tasks_).stopToken()
            );
            if (!endpoint)
            {
                std::terminate(); // Valid VFS and fixed nonzero admission capacity.
            }
            asset_reads_ = std::move(*endpoint);
        }
        return asset_reads_->port();
    }

    FrameworkResult<scene::SceneInstanceId> EditorContext::adoptScene(scene::SceneInstanceLease&& scene) noexcept
    {
        if (close_stage_ != ECloseStage::OPEN || !scene)
        {
            return cxx::unexpected(error::Error{Errors::EditorServiceUseOutsideProjectLifetime});
        }
        const auto id = scene.id();
        scenes_.push_back(std::move(scene));
        return id;
    }

    void EditorContext::close() noexcept
    {
        if (close_stage_ != ECloseStage::OPEN)
        {
            return;
        }
        close_stage_ = ECloseStage::SCENES;
        scene_retirements_.reserve(scenes_.size());
        for (auto& scene : scenes_)
        {
            scene_retirements_.push_back(scene.retire());
        }
        scenes_.clear();
    }

    ProjectResult<void> EditorContext::adoptSceneCatalog(
        const ProjectManifest& expected,
        std::vector<ProjectManifest::SceneRecord> scenes,
        std::optional<asset::AssetId> startup
    ) noexcept
    {
        if (close_stage_ != ECloseStage::OPEN || manifest_ != expected)
        {
            return cxx::unexpected(ProjectFailure{EProjectError::STALE_REVISION});
        }
        auto candidate = manifest_;
        candidate.scenes = std::move(scenes);
        candidate.startup_scene = startup;
        if (auto valid = validateProjectManifest(candidate); !valid)
        {
            return valid;
        }
        manifest_ = std::move(candidate);
        return {};
    }

    FrameworkResult<scene::InstanceRetirement> EditorContext::retireScene(scene::SceneInstanceId id) noexcept
    {
        const auto found = std::ranges::find(scenes_, id, &scene::SceneInstanceLease::id);
        if (found == scenes_.end())
        {
            const auto retired = std::ranges::find(scene_retirements_, id, &scene::InstanceRetirement::id);
            if (retired != scene_retirements_.end())
            {
                return *retired;
            }
            return cxx::unexpected(error::Error{Errors::EditorServiceUseOutsideProjectLifetime});
        }
        auto receipt = found->retire();
        scenes_.erase(found);
        scene_retirements_.push_back(receipt);
        return receipt;
    }

    bool EditorContext::finishClose() noexcept
    {
        if (close_stage_ == ECloseStage::OPEN)
        {
            return false;
        }
        if (close_stage_ == ECloseStage::SCENES)
        {
            for (const auto& retirement : scene_retirements_)
            {
                if (!retirement.complete())
                {
                    return false;
                }
            }
            // Retirement receipts now prove Scene cleanup completed before system destruction.
            tasks_.requestStop();
            asset_reads_.reset();
            close_stage_ = ECloseStage::TASKS;
        }
        // Without Scene borrows, accepted tasks already own their inputs/code independently of this Context.
        return scene_retirements_.empty() || tasks_.settled();
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
