#include <lux/engine/editor/scene/detail/SceneEditorImpl.hpp>
#include <lux/engine/editor/ui/scene/InspectorPane.hpp>
#include <lux/engine/editor/ui/scene/SceneContentElement.hpp>
#include <lux/engine/editor/ui/scene/SceneCreationPane.hpp>

namespace lux::editor::scene
{
    EditorResult<SceneSourceCodec::Source> SceneSourceCodec::decode(
        const lux::cxx::SharedBytes<>& bytes,
        std::stop_token stop
    ) noexcept
    {
        auto source = lux::scene::decodeScenePackage(bytes, stop);
        if (!source)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "scene.decode",
                static_cast<std::uint64_t>(source.error().code),
                {},
                source.error()
            });
        return std::move(*source);
    }

    EditorResult<void> SceneEditor::Impl::changeAsset(EAssetChange change, asset::AssetId id, bool reload)
    {
        const bool pending = reading_ || asset_status_.phase != EAssetEditPhase::IDLE || save.index() != 0 ||
                             placement.index() != 0 || run_status.state == EPlaybackState::PREPARING ||
                             run_status.state == EPlaybackState::STOPPING;
        if (pending)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.open"});
        if (change == EAssetChange::OPEN)
        {
            const auto* entry = editor_context_.project().asset(id);
            if (!entry || entry->kind != EProjectAssetKind::SCENE)
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.open"});
            if (source && source->scene->id() == id && !reload)
            {
                editor->setVisible(true);
                return {};
            }
        }
        auto finished = finishEditing();
        if (!finished)
            return finished;
        resume_after_change_ = run_status.state == EPlaybackState::RUNNING;
        if (resume_after_change_)
        {
            const auto paused = pauseRun(active_run_);
            if (!paused)
            {
                resume_after_change_ = false;
                return paused;
            }
        }
        asset_status_ = {EAssetEditPhase::REVIEW, change, id, {}};
        if (!history || persistence_.clean())
            startAssetChange();
        return {};
    }

    void SceneEditor::Impl::restorePlayback()
    {
        if (!resume_after_change_ || asset_status_.phase != EAssetEditPhase::IDLE)
            return;
        if (runSettled())
            resume_after_change_ = false;
        else if (run_status.state == EPlaybackState::PAUSED)
        {
            const auto resumed = resumeRun(active_run_);
            if (resumed)
                resume_after_change_ = false;
            else
                failure = resumed.error();
        }
    }

    void SceneEditor::Impl::assetFailure(EditorFailure failure)
    {
        candidate_editing_.reset();
        candidate_history_.reset();
        candidate_content_.reset();
        candidate_scene_.reset();
        candidate_.reset();
        copied_source_.reset();
        asset_status_.phase = EAssetEditPhase::IDLE;
        asset_status_.failure = std::move(failure);
        restorePlayback();
    }

    void SceneEditor::Impl::startAssetChange()
    {
        asset_status_.failure.reset();
        if (asset_status_.change == EAssetChange::OPEN)
        {
            const auto* entry = editor_context_.project().asset(asset_status_.target);
            if (!entry)
            {
                assetFailure({EEditorError::INVALID_ARGUMENT, "scene.open"});
                return;
            }
            auto admitted = editor_context_.execution().submit(
                {"Open scene source", "asset"},
                [&](process::TaskReporter reporter) noexcept {
                    return lux::editor::detail::readAssetSource(
                        editor_context_.project(),
                        *entry,
                        editor_context_.execution(),
                        reporter,
                        SceneSourceCodec{}
                    );
                },
                [this](process::TTaskResult<SceneSourceCodec::Source, EditorFailure>&& result) noexcept {
                    read_result_.emplace(lux::editor::detail::taskResult(std::move(result)));
                    reading_ = {};
                    completion_work_.request();
                }
            );
            if (!admitted)
            {
                assetFailure(
                    {EEditorError::EXECUTION_FAILURE,
                     "scene.open",
                     static_cast<std::uint64_t>(admitted.error()),
                     {},
                     admitted.error()}
                );
                return;
            }
            reading_ = std::move(*admitted);
            asset_status_.phase = EAssetEditPhase::READING;
        }
        else if (asset_status_.change == EAssetChange::NEW)
        {
            asset_status_.phase = EAssetEditPhase::CONFIGURING;
        }
        else if (asset_status_.change == EAssetChange::EXIT)
            asset_status_.phase = EAssetEditPhase::EXIT_READY;
        else
            asset_status_.phase = EAssetEditPhase::PREPARING;
    }

    EditorResult<void> SceneEditor::Impl::reviewAsset(EAssetChangeDecision choice, std::string_view path)
    {
        if (asset_status_.phase != EAssetEditPhase::REVIEW)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.review"});
        if (choice == EAssetChangeDecision::CANCEL)
        {
            asset_status_ = {};
            restorePlayback();
            return {};
        }
        if (choice == EAssetChangeDecision::DISCARD)
        {
            startAssetChange();
            return {};
        }
        auto saved =
            editor_context_.project().asset(source->scene->id()) ? requestSave("asset switch") : requestSaveAs(path);
        if (!saved)
        {
            asset_status_.failure = saved.error();
            return lux::cxx::unexpected(saved.error());
        }
        change_save_ = *saved;
        asset_status_.phase = EAssetEditPhase::SAVING;
        return {};
    }

    EditorResult<void> SceneEditor::Impl::prepareCandidate()
    try
    {
        auto assets = editor_context_.project().captureAssetReads();
        if (!assets)
            return lux::cxx::unexpected(assets.error());
        const auto identity = editor_context_.project().reference({});
        candidate_assets_ = {{identity.project_instance, 0}, identity.catalog_revision, std::move(*assets)};
        auto created = detail::instantiateScenePackage(
            runtime_,
            *candidate_,
            editor_context_.sceneRegistrations(),
            editor_context_.project().tasks(),
            editor_context_.renderRuntime(),
            editor_context_.renderResources(),
            candidate_assets_,

            true
        );
        if (!created)
            return lux::cxx::unexpected(created.error());
        auto viewport = candidate_viewport_;
        if (!viewport.valid())
        {
            // Resolve the sole renderer once at admission; never choose a different
            // provider from Registry order while a view is alive.
            const auto& description = candidate_->scene->data();
            for (std::size_t index{}; index < description.systemCount(); ++index)
            {
                const auto id = description.systemAt(index).instanceId();
                if (!lux::scene::RenderSceneState::find(readRegistry(created->id()), id))
                    continue;
                if (viewport.valid())
                    return lux::cxx::unexpected(EditorFailure{
                        EEditorError::INVALID_ARGUMENT,
                        "scene.viewport.ambiguous",
                        0,
                        "Select a render system for this scene"
                    });
                viewport = id;
            }
        }
        if (viewport.valid() && !lux::scene::RenderSceneState::find(readRegistry(created->id()), viewport))
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "scene.new.viewport"});
        auto edits = editing::EditHistory::create({kHistoryLimits, {}});
        if (!edits)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "scene.history", 0, {}, edits.error()}
            );
        candidate_viewport_ = viewport;
        candidate_scene_ = std::move(*created);
        candidate_history_ = std::move(*edits);
        candidate_persistence_.reset(*candidate_history_, asset_status_.change != EAssetChange::NEW);
        const auto& schemas = editor_context_.sceneRegistrations().components;
        candidate_content_.emplace(runtime_, candidate_scene_->id(), *candidate_, schemas);
        candidate_editing_.emplace(runtime_, candidate_scene_->id(), schemas, *candidate_history_);
        candidate_editing_->acceptsAsset = [this](asset::AssetId id, std::uint32_t magic) {
            const auto* row = editor_context_.project().catalogAsset(id);
            return row && row->magic == magic;
        };
        candidate_editing_->admission = [this] { return checkEditAdmission(); };
        candidate_editing_->changed = [this](const ComponentNotice& notice) {
            lux::editor::detail::reportSignalDelivery(
                editor->emit(editor->componentChanged, notice),
                "componentChanged"
            );
        };
        return {};
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.prepare"});
    }

    void SceneEditor::Impl::adoptCandidate()
    {
        scene_editing.reset();
        history.reset();
        content.reset();
        scene.reset();
        source = std::move(candidate_);
        scene = std::exchange(candidate_scene_, {});
        history = std::move(candidate_history_);
        persistence_ = std::move(candidate_persistence_);
        asset_source = std::move(candidate_assets_);
        viewport_system_ = candidate_viewport_;
        candidate_viewport_ = {};
        render_receipt = {};
        editor_camera = lux::simulation::ecs::NullEntity;
        selection_ = {(scene ? scene->id() : lux::scene::SceneInstanceId{}), lux::simulation::ecs::NullEntity, 0};
        resource_snapshot.reset();
        observed_history = {};
        changed_assets.clear();
        failure.emplace<std::monostate>();
        highlight_program.clear_keep_capacity();
        work_plane_program.clear_keep_capacity();
        highlight_pending = work_plane_pending = false;
        if (scene)
        {
            content.emplace(std::move(*candidate_content_));
            scene_editing.emplace(std::move(*candidate_editing_));
            candidate_editing_.reset();
            candidate_content_.reset();
            if (auto* render = inspectedRender())
                render_receipt = editor_context_.renderResources().sceneReceipt(render->resource);
        }
        content_->reopen();
        copied_source_.reset();
        replacing_ = false;
        asset_status_ = {};
        restorePlayback();
        editor->setVisible(true);
        lux::editor::detail::reportSignalDelivery(
            editor->emit(editor->selectionChanged, selection()),
            "selectionChanged"
        );
        lux::editor::detail::reportSignalDelivery(
            editor->emit(editor->objectsChanged, editing::Revision{}),
            "objectsChanged"
        );
    }

    void SceneEditor::Impl::adoptAssetResults()
    {
        if (copied_source_ && !candidate_)
        {
            auto* current = std::get_if<SceneSave>(&save);
            if (current && current->terminal())
            {
                if (std::holds_alternative<SaveSucceeded>(current->status()))
                {
                    candidate_ = copied_source_;
                    candidate_viewport_ = viewport_system_;
                    asset_status_.phase = EAssetEditPhase::PREPARING;
                }
                else
                {
                    copied_source_.reset();
                    asset_status_ = {};
                    restorePlayback();
                }
            }
            else if (current)
                if (const auto* failed = std::get_if<SaveRetryable>(&current->status()))
                    asset_status_.failure = failed->failure;
        }
        if (change_save_)
        {
            auto* current = std::get_if<SceneSave>(&save);
            if (current && current->terminal())
            {
                const bool succeeded = std::holds_alternative<SaveSucceeded>(current->status());
                static_cast<void>(acknowledgeSave(*change_save_));
                change_save_.reset();
                if (asset_status_.phase == EAssetEditPhase::SAVING)
                {
                    if (succeeded)
                        startAssetChange();
                    else
                        asset_status_.phase = EAssetEditPhase::REVIEW;
                }
            }
            else if (current)
                if (const auto* failed = std::get_if<SaveRetryable>(&current->status()))
                    asset_status_.failure = failed->failure;
        }
        if (read_result_)
        {
            auto result = std::move(*read_result_);
            read_result_.reset();
            if (!result)
            {
                assetFailure(result.error());
                return;
            }
            candidate_viewport_ = {};
            candidate_ = std::make_shared<const lux::scene::ScenePackage>(std::move(*result));
            asset_status_.phase = EAssetEditPhase::PREPARING;
        }
    }

    void SceneEditor::Impl::applyAssetChange()
    {
        if (asset_status_.phase == EAssetEditPhase::CONFIGURING && !creation_pane_)
        {
            auto pane = ui::createSceneCreationPane(*editor, editor_context_);
            if (!pane)
                assetFailure(pane.error());
            else
                creation_pane_ = std::move(*pane);
        }
        if (creation_pane_ && (asset_status_.phase != EAssetEditPhase::CONFIGURING))
            creation_pane_.reset();
        if (asset_status_.phase != EAssetEditPhase::PREPARING)
            return;
        if (candidate_ && !candidate_scene_)
        {
            auto prepared = prepareCandidate();
            if (!prepared)
            {
                assetFailure(prepared.error());
                if (asset_status_.change == EAssetChange::NEW)
                    asset_status_.phase = EAssetEditPhase::CONFIGURING;
                return;
            }
        }
        if (!replacing_)
        {
            if (!finishEditing() || (scene && !safe(scene->id())))
                return;
            if (inspector_ && !inspector_->content().clearTarget())
                return;
            content_->requestClose();
            if (!runSettled())
                static_cast<void>(run_start_ ? cancelRun(run_status.request) : stopRun(run_status.id));
            replacing_ = true;
        }
        if (!runSettled() || content_->closeStatus().state != ECloseState::CLOSED)
            return;
        if (history && !history->close())
            return;
        adoptCandidate();
    }

    EditorResult<void> SceneEditor::openAsset(asset::AssetId id)
    {
        return impl_->changeAsset(EAssetChange::OPEN, id);
    }

    EditorResult<SaveRequestId> SceneEditor::Impl::requestSaveAs(std::string_view path)
    {
        const bool invalid_phase =
            asset_status_.phase != EAssetEditPhase::IDLE && asset_status_.phase != EAssetEditPhase::REVIEW;
        if (!source || !history || save.index() != 0 || invalid_phase || next_save == UINT64_MAX)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.save-as"});
        if (!editor_context_.project().writable())
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "scene.save-as"});
        auto finished = finishEditing();
        if (!finished)
            return lux::cxx::unexpected(finished.error());
        const bool copying = editor_context_.project().asset(source->scene->id()) != nullptr;
        auto identity = source->scene->id();
        if (copying)
        {
            std::random_device seed;
            std::mt19937 random(seed());
            identity = asset::AssetId{uuids::uuid_random_generator{random}()};
        }
        auto target = lux::editor::detail::newAssetSaveTarget(
            editor_context_.project(),
            identity,
            EProjectAssetKind::SCENE,
            path
        );
        if (!target)
            return lux::cxx::unexpected(target.error());
        auto captured = captureSource();
        if (!captured)
            return lux::cxx::unexpected(captured.error());
        auto copy = copying ? std::make_shared<lux::scene::ScenePackage>() : nullptr;
        auto ticket = persistence_.capture(!copying);
        if (!ticket)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.save-as.ticket", 0, {}, ticket.error()}
            );
        const SaveRequestId id{history->id(), next_save++};
        save.emplace<SceneSave>(
            id,
            *ticket,
            history->view()->snapshot.revision,
            std::move(*target),
            SceneSaveCapture{std::move(*captured), copying ? identity : asset::AssetId{}, copy},
            editor_context_.project(),
            editor_context_.execution(),
            persistence_,
            completion_work_.requester()
        );
        if (copying)
        {
            copied_source_ = std::move(copy);
            resume_after_change_ = run_status.state == EPlaybackState::RUNNING;
            if (resume_after_change_)
            {
                const auto paused = pauseRun(active_run_);
                if (!paused)
                {
                    resume_after_change_ = false;
                    failure = paused.error();
                }
            }
            asset_status_ = {EAssetEditPhase::SAVING};
        }
        return id;
    }

    EditorResult<SaveRequestId> SceneEditor::requestSaveAs(std::string_view path)
    {
        return impl_->requestSaveAs(path);
    }
    EditorResult<void> SceneEditor::reloadAsset()
    {
        return impl_->changeAsset(EAssetChange::OPEN, assetId(), true);
    }
    EditorResult<void> SceneEditor::clearAsset()
    {
        return impl_->changeAsset(EAssetChange::CLEAR);
    }
    EditorResult<void> SceneEditor::prepareExit()
    {
        return impl_->changeAsset(EAssetChange::EXIT);
    }
    void SceneEditor::cancelExit() noexcept
    {
        if (impl_->asset_status_.change == EAssetChange::EXIT)
        {
            impl_->asset_status_ = {};
            impl_->restorePlayback();
        }
    }
    EditorResult<void> SceneEditor::reviewAsset(EAssetChangeDecision choice, std::string_view path)
    {
        return impl_->reviewAsset(choice, path);
    }
    const AssetEditStatus& SceneEditor::assetStatus() const noexcept
    {
        return impl_->asset_status_;
    }
    asset::AssetId SceneEditor::assetId() const noexcept
    {
        return impl_->source ? impl_->source->scene->id() : asset::AssetId{};
    }
}

namespace lux::editor::scene
{
    EditorResult<void> SceneEditor::newAsset()
    {
        return impl_->changeAsset(EAssetChange::NEW);
    }
    void SceneEditor::cancelNewAsset() noexcept
    {
        if (impl_->asset_status_.phase == EAssetEditPhase::CONFIGURING)
        {
            impl_->asset_status_ = {};
            impl_->restorePlayback();
        }
    }
    EditorResult<void> SceneEditor::createAsset(
        std::string_view name,
        std::span<const lux::world::WorldDataSchemaId> schemas,
        std::shared_ptr<const lux::simulation::SimulationDescription> simulation,
        const lux::scene::SceneDescription& description,
        lux::system::SystemInstanceId viewport
    )
    {
        if (impl_->asset_status_.phase != EAssetEditPhase::CONFIGURING || !simulation)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "scene.new"});
        std::random_device seed;
        std::mt19937 random(seed());
        auto source = lux::scene::createScenePackage(
            asset::AssetId{uuids::uuid_random_generator{random}()},
            name,
            schemas,
            std::move(simulation),
            description
        );
        if (!source)
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "scene.new", 0, {}, source.error()}
            );
        impl_->candidate_ = std::make_shared<const lux::scene::ScenePackage>(std::move(*source));
        impl_->candidate_viewport_ = viewport;
        impl_->asset_status_.failure.reset();
        impl_->asset_status_.phase = EAssetEditPhase::PREPARING;
        return {};
    }
}
