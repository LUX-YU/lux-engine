#include <lux/engine/editor/material/MaterialEditorImpl.hpp>
#include <random>

namespace lux::editor::material
{
    namespace
    {
        constexpr editing::HistoryLimits kLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};
        EditorFailure historyFailure(const editing::EditFailure& failure)
        {
            return {
                EEditorError::INVALID_STATE,
                "material.history",
                static_cast<std::uint64_t>(failure.code),
                failure.message.data(),
                failure
            };
        }
        asset::AssetId newIdentity()
        {
            std::random_device seed;
            std::mt19937 random(seed());
            return asset::AssetId{uuids::uuid_random_generator{random}()};
        }
    }

    EditorResult<MaterialSourceCodec::Source> MaterialSourceCodec::decode(
        const lux::cxx::SharedBytes<>& bytes,
        std::stop_token
    ) noexcept
    {
        auto source = lux::material::decodeMaterialSource({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        if (!source)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "material.decode",
                static_cast<std::uint64_t>(source.error().code),
                source.error().field,
                source.error()
            });
        return std::move(*source);
    }

    EditorResult<void> MaterialEditor::Impl::changeAsset(EAssetChange change, asset::AssetId id, bool reload)
    {
        if (busy_ || asset_status_.phase != EAssetEditPhase::IDLE || save_.index() != 0 || saved_history_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.open"});
        if (change == EAssetChange::OPEN)
        {
            const auto* row = editor_context_.project().asset(id);
            if (!row || row->kind != EProjectAssetKind::MATERIAL_GRAPH)
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::INVALID_ARGUMENT,
                    "material.open",
                    0,
                    "This asset has no editable material graph source"
                });
            if (id == source_.id && !reload)
            {
                editor_->setVisible(true);
                return {};
            }
        }
        const auto finished = finishEditing();
        if (!finished)
            return finished;
        const auto current =
            history_ ? history_->view()
                     : editing::EditResult<editing::HistoryView>{
                           lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET))
                       };
        if (history_ && !current)
            return lux::cxx::unexpected(historyFailure(current.error()));
        asset_status_ = {EAssetEditPhase::REVIEW, change, id, {}};
        if (auto* job = std::get_if<Compilation>(&compilation_))
            compile_task_.requestStop();
        if (!history_ || current->snapshot.clean)
            startAssetChange();
        return {};
    }

    void MaterialEditor::Impl::assetFailure(EditorFailure failure)
    {
        candidate_.reset();
        candidate_history_.reset();
        asset_status_.phase = EAssetEditPhase::IDLE;
        asset_status_.failure = std::move(failure);
    }

    void MaterialEditor::Impl::startAssetChange()
    {
        asset_status_.failure.reset();
        if (asset_status_.change == EAssetChange::OPEN)
        {
            const auto* row = editor_context_.project().asset(asset_status_.target);
            if (!row || row->kind != EProjectAssetKind::MATERIAL_GRAPH)
            {
                assetFailure({EEditorError::INVALID_ARGUMENT, "material.open"});
                return;
            }
            auto admitted = editor_context_.execution().submit(
                {"Open material source", "asset"},
                [&](process::TaskReporter reporter) noexcept {
                    return lux::editor::detail::readAssetSource(
                        editor_context_.project(),
                        *row,
                        editor_context_.execution(),
                        reporter,
                        MaterialSourceCodec{}
                    );
                },
                [this](process::TTaskResult<MaterialSourceCodec::Source, EditorFailure>&& result) noexcept {
                    read_result_.emplace(lux::editor::detail::taskResult(std::move(result)));
                    reading_ = {};
                    completion_work_.request();
                }
            );
            if (!admitted)
            {
                assetFailure(
                    {EEditorError::EXECUTION_FAILURE,
                     "material.open",
                     static_cast<std::uint64_t>(admitted.error()),
                     {},
                     admitted.error()}
                );
                return;
            }
            reading_ = std::move(*admitted);
            asset_status_.phase = EAssetEditPhase::READING;
        }
        else if (asset_status_.change == EAssetChange::EXIT)
            asset_status_.phase = EAssetEditPhase::EXIT_READY;
        else
        {
            if (asset_status_.change == EAssetChange::NEW)
                candidate_.emplace(newIdentity(), "Untitled material", lux::material::MaterialGraph{});
            asset_status_.phase = EAssetEditPhase::PREPARING;
        }
    }

    EditorResult<void> MaterialEditor::Impl::reviewAsset(EAssetChangeDecision choice, std::string_view path)
    {
        if (asset_status_.phase != EAssetEditPhase::REVIEW)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "material.review"});
        if (choice == EAssetChangeDecision::CANCEL)
        {
            asset_status_ = {};
            return {};
        }
        if (choice == EAssetChangeDecision::DISCARD)
        {
            startAssetChange();
            return {};
        }
        auto save_ = editor_context_.project().asset(source_.id) ? requestSave("asset switch") : requestSaveAs(path);
        if (!save_)
        {
            asset_status_.failure = save_.error();
            return lux::cxx::unexpected(save_.error());
        }
        change_save_ = *save_;
        asset_status_.phase = EAssetEditPhase::SAVING;
        return {};
    }

    EditorResult<SaveRequestId> MaterialEditor::Impl::requestSaveAs(std::string_view path)
    {
        const bool invalid_phase =
            asset_status_.phase != EAssetEditPhase::IDLE && asset_status_.phase != EAssetEditPhase::REVIEW;
        if (busy_ || invalid_phase || save_.index() != 0 || !history_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.save-as"});
        if (!editor_context_.project().writable() || !editor_context_.execution().blocking())
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "material.save-as"});
        auto finished = finishEditing();
        if (!finished)
            return lux::cxx::unexpected(finished.error());
        const bool copying = editor_context_.project().asset(source_.id) != nullptr;
        const auto identity = copying ? newIdentity() : source_.id;
        auto target =
            detail::newAssetSaveTarget(editor_context_.project(), identity, EProjectAssetKind::MATERIAL_GRAPH, path);
        if (!target)
            return lux::cxx::unexpected(target.error());
        std::unique_ptr<editing::EditHistory> next_history;
        if (copying)
        {
            auto created = editing::EditHistory::create({kLimits, {}, true});
            if (!created)
                return lux::cxx::unexpected(historyFailure(created.error()));
            next_history = std::move(*created);
        }
        if (next_save_ == UINT64_MAX)
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "material.save-as"});
        lux::material::MaterialSource capture{identity, source_.name, source_.graph.clone()};
        auto ticket = history_->beginSave();
        if (!ticket)
            return lux::cxx::unexpected(historyFailure(ticket.error()));
        const SaveRequestId id{history_->id(), next_save_++};
        save_.emplace<MaterialSave>(
            id,
            *ticket,
            history_->view()->snapshot.revision,
            std::move(*target),
            std::move(capture),
            editor_context_.project(),
            editor_context_.execution(),
            *history_,
            completion_work_.requester()
        );
        if (copying)
        {
            saved_identity_ = identity;
            saved_history_ = std::move(next_history);
            if (auto* job = std::get_if<Compilation>(&compilation_))
                compile_task_.requestStop();
        }
        return id;
    }

    void MaterialEditor::Impl::adoptAssetResults()
    {
        auto* pending_save = std::get_if<MaterialSave>(&save_);
        if (saved_history_ && pending_save && pending_save->terminal())
        {
            if (std::holds_alternative<SaveSucceeded>(pending_save->status()))
            {
                if (const auto* job = std::get_if<Compilation>(&compilation_); job && static_cast<bool>(compile_task_))
                    return;
                auto reset = resetPreview();
                if (!reset)
                {
                    if (reset.error().code == EEditorError::BUSY)
                    {
                        completion_deferred_ = true;
                        return;
                    }
                    asset_status_.failure = reset.error();
                    return;
                }
                if (!history_->close())
                    return;
                compilation_.emplace<std::monostate>();
                source_.id = saved_identity_;
                history_ = std::move(saved_history_);
            }
            else
                saved_history_.reset();
            saved_identity_ = {};
        }
        if (asset_status_.phase != EAssetEditPhase::SAVING && change_save_ && pending_save &&
            pending_save->terminal() && !saved_history_)
        {
            static_cast<void>(acknowledgeSave(*change_save_));
            change_save_.reset();
        }
        if (asset_status_.phase == EAssetEditPhase::SAVING && change_save_)
        {
            const auto status = saveStatus(*change_save_);
            if (!status)
            {
                assetFailure(status.error());
                change_save_.reset();
                return;
            }
            if (const auto* failed = std::get_if<SaveRetryable>(&*status))
            {
                asset_status_.failure = failed->failure;
                return;
            }
            if (!pending_save->terminal() || saved_history_)
                return;
            const bool succeeded = std::holds_alternative<SaveSucceeded>(*status);
            static_cast<void>(acknowledgeSave(*change_save_));
            change_save_.reset();
            if (succeeded)
                startAssetChange();
            else
                asset_status_.phase = EAssetEditPhase::REVIEW;
        }
        if (read_result_)
        {
            auto decoded = std::move(*read_result_);
            read_result_.reset();
            if (!decoded)
            {
                assetFailure(std::move(decoded.error()));
                return;
            }
            candidate_.emplace(std::move(*decoded));
            asset_status_.phase = EAssetEditPhase::PREPARING;
        }
    }

    void MaterialEditor::Impl::applyAssetChange()
    {
        if (asset_status_.phase != EAssetEditPhase::PREPARING)
            return;
        if (const auto* job = std::get_if<Compilation>(&compilation_); job && static_cast<bool>(compile_task_))
            return;
        if (candidate_ && !candidate_history_)
        {
            auto created = editing::EditHistory::create({kLimits, {}, asset_status_.change == EAssetChange::OPEN});
            if (!created)
            {
                assetFailure(historyFailure(created.error()));
                return;
            }
            candidate_history_ = std::move(*created);
        }
        auto reset = resetPreview();
        if (!reset)
        {
            if (reset.error().code != EEditorError::BUSY)
                assetFailure(reset.error());
            return;
        }
        if (history_ && !history_->close())
            return;
        compilation_.emplace<std::monostate>();
        history_ = std::move(candidate_history_);
        source_ = candidate_ ? std::move(*candidate_) : lux::material::MaterialSource{};
        candidate_.reset();
        editor_->setTitle(source_.id.isNull() ? "Material Editor" : source_.name);
        asset_status_ = {};
        editor_->setVisible(true);
    }

    EditorResult<void> MaterialEditor::openAsset(asset::AssetId id)
    {
        return impl_->changeAsset(EAssetChange::OPEN, id);
    }
    EditorResult<void> MaterialEditor::newAsset()
    {
        return impl_->changeAsset(EAssetChange::NEW);
    }
    EditorResult<void> MaterialEditor::reloadAsset()
    {
        return impl_->changeAsset(EAssetChange::OPEN, impl_->source_.id, true);
    }
    EditorResult<void> MaterialEditor::clearAsset()
    {
        return impl_->changeAsset(EAssetChange::CLEAR);
    }
    EditorResult<void> MaterialEditor::prepareExit()
    {
        return impl_->changeAsset(EAssetChange::EXIT);
    }
    void MaterialEditor::cancelExit() noexcept
    {
        if (impl_->asset_status_.change == EAssetChange::EXIT)
        {
            impl_->asset_status_ = {};
        }
    }
    EditorResult<void> MaterialEditor::reviewAsset(EAssetChangeDecision choice, std::string_view path)
    {
        return impl_->reviewAsset(choice, path);
    }
    const AssetEditStatus& MaterialEditor::assetStatus() const noexcept
    {
        return impl_->asset_status_;
    }
    asset::AssetId MaterialEditor::assetId() const noexcept
    {
        return impl_->source_.id;
    }
    EditorResult<SaveRequestId> MaterialEditor::requestSaveAs(std::string_view path)
    {
        return impl_->requestSaveAs(path);
    }
}
