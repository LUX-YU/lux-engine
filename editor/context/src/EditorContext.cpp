#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <algorithm>
#include <lux/engine/Engine.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectErrors.hpp>
#include <lux/engine/editor/detail/PreparedProject.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/process/assets/AssetRepository.hpp>
#include <lux/engine/scene/SceneError.hpp>

namespace lux::editor
{
    EditorContext::EditorContext(
        engine::Engine& engine,
        detail::PreparedProject prepared,
        EditorComposition&& composition
    ) noexcept
        : engine_(engine), project_(std::move(prepared.description)), manifest_(std::move(prepared.manifest)),
          plugins_(std::move(prepared.plugins)), registrations_(std::move(prepared.registrations)),
          ui_(std::move(composition.ui_)), scene_profiles_(std::move(composition.scene_profiles_)),
          component_editors_(std::move(composition.component_editors_)),
          scene_configuration_editors_(std::move(composition.scene_configuration_editors_)),
          extensions_(std::move(composition.extensions_).publish()), services_(std::move(composition.services_)),
          tasks_(execution::ExecutionRuntime::instance())
    {
    }

    const ComponentEditor* EditorContext::componentEditor(cxx::type_info type) const noexcept
    {
        const auto found = std::ranges::find(component_editors_, type, &ComponentEditor::type);
        return found == component_editors_.end() ? nullptr : &*found;
    }

    const SceneConfigurationEditor* EditorContext::sceneConfigurationEditor(cxx::type_info type) const noexcept
    {
        const auto found = std::ranges::find(scene_configuration_editors_, type, &SceneConfigurationEditor::type);
        return found == scene_configuration_editors_.end() ? nullptr : &*found;
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
        engine::Engine& engine,
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

    FrameworkResult<process::assets::AssetRepository*> EditorContext::prepareAssets() noexcept
    {
        if (!loaded_assets_)
        {
            auto repository = process::assets::AssetRepository::create(assets_.view(), {256U << 20U, 512U << 20U, 32}
            );
            if (!repository)
            {
                return cxx::unexpected(repository.error());
            }
            loaded_assets_ = std::move(*repository);
        }
        return loaded_assets_.get();
    }

    FrameworkResult<scene::SceneInstanceId> EditorContext::createScene(scene::SceneRuntime::Builder& builder) noexcept
    {
        if (close_stage_ != ECloseStage::OPEN)
        {
            return cxx::unexpected(error::Error{Errors::EditorServiceUseOutsideProjectLifetime});
        }
        auto created = builder.build();
        if (created)
        {
            scenes_.push_back(*created);
            static_cast<void>(emit(sceneAdded, *created));
        }
        return created ? FrameworkResult<scene::SceneInstanceId>{*created}
                       : cxx::unexpected(scene::toError(created.error()));
    }

    exec::task<FrameworkResult<scene::SceneInstanceId>>
    EditorContext::createSceneAsync(scene::SceneRuntime::Builder& builder) noexcept
    {
        if (close_stage_ != ECloseStage::OPEN)
        {
            co_return cxx::unexpected(error::Error{Errors::EditorServiceUseOutsideProjectLifetime});
        }
        auto created = co_await builder.buildAsync();
        if (!created)
        {
            co_return cxx::unexpected(scene::toError(created.error()));
        }
        if (close_stage_ != ECloseStage::OPEN)
        {
            auto retired = engine_.sceneRuntime().retireInstance(*created);
            if (!retired)
            {
                co_return cxx::unexpected(scene::toError(retired.error()));
            }
            auto closed = co_await retired->whenComplete();
            if (!closed)
            {
                co_return cxx::unexpected(closed.error());
            }
            co_return cxx::unexpected(error::Error{Errors::EditorServiceUseOutsideProjectLifetime});
        }
        scenes_.push_back(*created);
        static_cast<void>(emit(sceneAdded, *created));
        co_return *created;
    }

    void EditorContext::close() noexcept
    {
        if (close_stage_ != ECloseStage::OPEN)
        {
            return;
        }
        close_stage_ = ECloseStage::SERVICES;
        services_.requestClose();
    }

    FrameworkResult<void> EditorContext::retireScenes() noexcept
    {
        scene_retirements_.reserve(scene_retirements_.size() + scenes_.size());
        // Remove each membership only after Runtime accepts its retirement. A failed suffix remains owned.
        while (!scenes_.empty())
        {
            auto retired = engine_.sceneRuntime().retireInstance(scenes_.back());
            if (!retired)
            {
                return cxx::unexpected(scene::toError(retired.error()));
            }
            scene_retirements_.push_back(std::move(*retired));
            scenes_.pop_back();
        }
        close_stage_ = ECloseStage::SCENES;
        return {};
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

    FrameworkResult<scene::SceneCloseReceipt> EditorContext::retireScene(scene::SceneInstanceId id) noexcept
    {
        const auto found = std::ranges::find(scenes_, id);
        if (found == scenes_.end())
        {
            const auto retired = std::ranges::find(scene_retirements_, id, &scene::SceneCloseReceipt::id);
            if (retired != scene_retirements_.end())
            {
                return *retired;
            }
            return cxx::unexpected(error::Error{Errors::EditorServiceUseOutsideProjectLifetime});
        }
        auto retired = engine_.sceneRuntime().retireInstance(id);
        if (!retired)
        {
            return cxx::unexpected(scene::toError(retired.error()));
        }
        auto receipt = std::move(*retired);
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
        if (close_stage_ == ECloseStage::SERVICES)
        {
            if (!services_.closeComplete())
            {
                return false;
            }
            if (!retireScenes())
            {
                return false;
            }
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
            if (loaded_assets_)
            {
                loaded_assets_->requestRetirement();
            }
            close_stage_ = ECloseStage::TASKS;
        }
        // Without Scene borrows, accepted tasks already own their inputs/code independently of this Context.
        return tasks_.settled() && !loaded_assets_;
    }

    exec::task<FrameworkResult<void>> EditorContext::closeAsync() noexcept
    {
        close();
        if (close_stage_ == ECloseStage::SERVICES)
        {
            auto services = co_await services_.closeAsync();
            if (!services)
            {
                co_return cxx::unexpected(services.error());
            }
            if (auto retired = retireScenes(); !retired)
            {
                co_return retired;
            }
        }
        for (const auto& retirement : scene_retirements_)
        {
            auto retired =
                co_await stdexec::continues_on(
                    retirement.whenComplete(),
                    execution::ExecutionRuntime::instance().mainScheduler()
                );
            if (!retired)
            {
                co_return cxx::unexpected(retired.error());
            }
        }
        tasks_.requestStop();
        close_stage_ = ECloseStage::TASKS;
        co_await stdexec::continues_on(tasks_.whenSettled(), execution::ExecutionRuntime::instance().mainScheduler());
        if (loaded_assets_)
        {
            loaded_assets_->requestRetirement();
            auto settled = co_await loaded_assets_->whenQuiescent();
            if (!settled)
            {
                co_return settled;
            }
            auto retired = co_await loaded_assets_->retire();
            if (!retired)
            {
                co_return retired;
            }
            loaded_assets_.reset();
        }
        co_return FrameworkResult<void>{};
    }

    exec::task<FrameworkResult<void>> EditorContext::recoverClose() noexcept
    {
        if (close_stage_ == ECloseStage::OPEN)
        {
            co_return co_await closeAsync();
        }
        auto recovered = co_await services_.recoverClose();
        if (!recovered)
        {
            co_return cxx::unexpected(recovered.error());
        }
        for (const auto& receipt : scene_retirements_)
        {
            if (receipt.failure().type != 0)
            {
                auto retried = engine_.sceneRuntime().retryClose(receipt.id());
                if (!retried)
                {
                    co_return cxx::unexpected(scene::toError(retried.error()));
                }
            }
        }
        co_return co_await closeAsync();
    }

    FrameworkResult<std::unique_ptr<EditorContext>> detail::createEditorContext(
        engine::Engine& engine,
        PreparedProject prepared,
        EditorComposition&& composition
    ) noexcept
    {
        return EditorContext::create(engine, std::move(prepared), std::move(composition));
    }
} // namespace lux::editor
