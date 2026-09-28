#include <lux/engine/editor/metadata/AssetEditorRegistration.hpp>
#include <lux/engine/editor/material/MaterialEditorImpl.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/ui/HistoryCommands.hpp>
#include <algorithm>
#include <lux/engine/editor/material/MaterialPreview.hpp>
#include <cmath>
#include <lux/engine/editor/detail/AssetSave.hpp>
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

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
                {},
                failure
            };
        }

        struct BusyGuard final
        {
            bool& busy_;
            explicit BusyGuard(bool& value) : busy_(value)
            {
                busy_ = true;
            }
            ~BusyGuard()
            {
                busy_ = false;
            }
        };
    } // namespace

    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::edit(
        std::vector<VMaterialEdit> edits,
        std::string label,
        std::vector<lux::material::NodeId>* inserted
    )
    {
        if (auto admitted = canEdit(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        BusyGuard guard(busy_);
        auto prepared = prepareMaterialEdit(
            source_,
            history_->view()->snapshot.current,
            std::move(edits),
            std::move(label),
            contracts::CodeLease::builtin(),
            {this,
             [](void* raw, const editing::CommitInfo& info) noexcept {
                 auto& owner = *static_cast<Impl*>(raw);
                 lux::editor::detail::reportSignalDelivery(
                     owner.editor_->emit(owner.editor_->contentChanged, info.revision),
                     "MaterialEditor::contentChanged"
                 );
             }},
            kLimits.max_staging_bytes
        );
        if (!prepared)
        {
            const auto& error = prepared.error();
            if (error.code == EMaterialEditError::HISTORY)
                return lux::cxx::unexpected(error.history);
            const auto code = error.code == EMaterialEditError::BUDGET ? editing::EEditError::STAGING_LIMIT
                              : error.code == EMaterialEditError::INVALID_VALUE
                                  ? editing::EEditError::INVALID_ARGUMENT
                                  : editing::EEditError::PRECONDITION_FAILED;
            const auto domain = error.code == EMaterialEditError::INVALID_GRAPH
                                    ? static_cast<std::uint64_t>(error.graph.code)
                                    : static_cast<std::uint64_t>(error.code);
            return lux::cxx::unexpected(editing::makeEditFailure(code, domain));
        }
        auto result = history_->execute(prepared->operation);
        if (result && inserted)
            *inserted = std::move(prepared->inserted);
        return result;
    }

    editing::EditResult<void> MaterialEditor::Impl::canEdit() const noexcept
    {
        if (!history_)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET));
        const bool is_changing = asset_status_.phase != EAssetEditPhase::IDLE || bool(saved_history_);
        const bool is_busy = busy_ || (is_changing && !finishing_interaction_);
        const bool is_read_only = !editor_context_.project().writable();
        if (is_busy || is_read_only)
            return lux::cxx::unexpected(
                editing::makeEditFailure(is_busy ? editing::EEditError::BUSY : editing::EEditError::BLOCKED_BY_HOST)
            );
        return {};
    }

    MaterialEditor::MaterialEditor(
        lux::ui::Root& parent,
        lux::ui::PaneId id,
        std::unique_ptr<Impl> data,
        EditorResult<void>& status
    )
        : lux::ui::Pane(parent, std::move(id), lux::ui::PaneTypeId{kMaterialEditorType}, "Material Editor"),
          impl_(std::move(data))
    {
        impl_->editor_ = this;
        impl_->createContent(status);
        impl_->close_connection_ = lux::editor::detail::takeConnection(
            lux::object::LuxObject::connect(
                this,
                &lux::ui::Pane::closeRequested,
                [this]() noexcept { impl_->hide_requested_ = true; }
            ),
            status
        );
    }
    MaterialEditor::~MaterialEditor() = default;

    EditorResult<std::unique_ptr<MaterialEditor>> MaterialEditor::create(
        lux::ui::Root& parent,
        lux::ui::PaneId id,
        EditorContext& context
    ) noexcept
    try
    {
        auto data = std::make_unique<Impl>(context);
        EditorResult<void> status;
        auto result =
            std::unique_ptr<MaterialEditor>(new MaterialEditor(parent, std::move(id), std::move(data), status));
        if (!status)
            return lux::cxx::unexpected(status.error());
        return result;
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "material.create"});
    }

    const lux::material::MaterialSource& MaterialEditor::Impl::source() const noexcept
    {
        return this->source_;
    }
    ProjectStorage& MaterialEditor::Impl::project() noexcept
    {
        return editor_context_.project();
    }

    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::rename(std::string_view name)
    {
        std::vector<VMaterialEdit> edits;
        edits.push_back(MaterialRename{std::string(name)});
        return edit(std::move(edits), "Rename material");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::setConstant(
        lux::material::NodeId node,
        const std::array<float, 4>& value
    )
    {
        std::vector<VMaterialEdit> edits;
        edits.push_back(MaterialSetConstant{node, value});
        return edit(std::move(edits), "Change constant");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::setShadingModel(lux::rdesc::ELightingTechnique value
    )
    {
        std::vector<VMaterialEdit> edits;
        edits.push_back(MaterialSetShading{value});
        return edit(std::move(edits), "Change shading model");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::setRenderState(lux::material::RenderState value)
    {
        std::vector<VMaterialEdit> edits;
        edits.push_back(MaterialSetRenderState{value});
        return edit(std::move(edits), "Change render state");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::setTextureSlots(
        editing::StateId base,
        std::span<const lux::material::TextureSlotDecl> slots
    )
    {
        if (auto admitted = canEdit(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        if (history_->view()->snapshot.current != base)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        const auto& previous = source_.graph.texture_slots;
        for (std::size_t index{}; index < slots.size(); ++index)
        {
            const auto asset = slots[index].texture;
            if (!asset.isNull() && (index >= previous.size() || asset != previous[index].texture))
            {
                const auto* entry = editor_context_.project().catalogAsset(asset);
                if (!entry || entry->magic != lux::asset::TextureAsset::primary_magic)
                    return lux::cxx::unexpected(editing::makeEditFailure(
                        editing::EEditError::INVALID_ARGUMENT,
                        0,
                        "The selected asset is not a texture in this project"
                    ));
            }
        }
        std::vector<VMaterialEdit> edits;
        edits.push_back(MaterialSetTextureSlots{{slots.begin(), slots.end()}});
        return edit(std::move(edits), "Edit texture slots");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::setParameterSlots(
        editing::StateId base,
        std::span<const lux::material::ParamSlotDecl> slots
    )
    {
        if (auto admitted = canEdit(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        if (history_->view()->snapshot.current != base)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        std::vector<VMaterialEdit> edits;
        edits.push_back(MaterialSetParameterSlots{{slots.begin(), slots.end()}});
        return edit(std::move(edits), "Edit parameter slots");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::replaceNode(
        editing::StateId base,
        std::unique_ptr<lux::material::Node>& node
    )
    {
        if (auto admitted = canEdit(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        if (history_->view()->snapshot.current != base)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        std::vector<VMaterialEdit> edits;
        edits.push_back(MaterialReplaceNode{contracts::CodeLease::builtin(), node ? node->clone() : nullptr});
        auto result = edit(std::move(edits), "Edit node properties");
        if (result)
            node.reset();
        return result;
    }
    editing::EditResult<lux::material::NodeId> MaterialEditor::Impl::insertNode(
        std::unique_ptr<lux::material::Node>& node,
        lux::graph::GraphNodeLayout placement
    )
    {
        if (auto admitted = canEdit(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        if (!node || node->id().valid())
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        std::vector<VMaterialEdit> edits;
        edits.push_back(MaterialInsertNode{contracts::CodeLease::builtin(), node->clone(), placement});
        std::vector<lux::material::NodeId> inserted;
        auto result = edit(std::move(edits), "Add node", &inserted);
        if (!result)
            return lux::cxx::unexpected(result.error());
        node.reset();
        return inserted.front();
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::removeNodes(
        std::span<const lux::material::NodeId> nodes,
        std::span<const lux::graph::LinkRecord> links
    )
    {
        std::vector<VMaterialEdit> edits;
        // Explicit links first: erasing either endpoint then removes its remaining incident links.
        for (const auto& link : links)
            edits.push_back(MaterialDisconnect{link.from, link.to});
        for (const auto node : nodes)
            edits.push_back(MaterialEraseNode{node});
        return edit(std::move(edits), "Remove nodes");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::connect(
        lux::material::PinId from,
        lux::material::PinId to
    )
    {
        std::vector<VMaterialEdit> edits;
        edits.push_back(MaterialConnect{from, to});
        return edit(std::move(edits), "Connect pins");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::disconnect(
        lux::material::PinId from,
        lux::material::PinId to
    )
    {
        std::vector<VMaterialEdit> edits;
        edits.push_back(MaterialDisconnect{from, to});
        return edit(std::move(edits), "Disconnect pins");
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::moveNode(
        lux::material::NodeId node,
        lux::graph::GraphNodeLayout value
    )
    {
        const lux::graph::GraphLayoutEntry entry{node, value};
        return moveNodes(std::span{&entry, 1});
    }
    editing::EditResult<editing::ApplyResult> MaterialEditor::Impl::moveNodes(
        std::span<const lux::graph::GraphLayoutEntry> entries
    )
    {
        std::vector<VMaterialEdit> edits;
        for (const auto& entry : entries)
        {
            const bool duplicate = std::ranges::any_of(edits, [&](const auto& edit) {
                return std::get<MaterialPlaceNode>(edit).node == entry.node;
            });
            if (duplicate)
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
            edits.push_back(MaterialPlaceNode{entry.node, entry.layout});
        }
        return edit(std::move(edits), "Move nodes");
    }

    EditorResult<SaveRequestId> MaterialEditor::Impl::requestSave(std::string origin)
    {
        if (!history_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "material.empty"});
        if (asset_status_.phase != EAssetEditPhase::IDLE && asset_status_.phase != EAssetEditPhase::REVIEW)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.save"});
        if (!editor_context_.project().writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "material.save"});
        }
        if (origin.empty() || !editor_context_.execution().blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "material.save"});
        }
        if (this->busy_ || this->save_.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.save"});
        }
        if (this->next_save_ == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "material.save"});
        }
        lux::material::MaterialSource capture{this->source_.id, this->source_.name, this->source_.graph.clone()};
        auto target = lux::editor::detail::captureAssetSaveTarget(editor_context_.project(), this->source_.id);
        if (!target)
            return lux::cxx::unexpected(target.error());
        auto ticket = this->persistence_.capture();
        if (!ticket)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::BUSY, "asset.save.ticket", static_cast<std::uint64_t>(ticket.error())}
            );
        }
        const SaveRequestId id{this->history_->id(), this->next_save_++};
        this->save_.emplace<MaterialSave>(
            id,
            *ticket,
            this->history_->view()->snapshot.revision,
            std::move(*target),
            std::move(capture),
            editor_context_.project(),
            editor_context_.execution(),
            this->persistence_,
            completion_work_.requester()
        );
        return id;
    }
    std::span<const SaveRequestId> MaterialEditor::Impl::saveRequests() const noexcept
    {
        const auto* save_ = std::get_if<MaterialSave>(&this->save_);
        return save_ ? save_->requests() : std::span<const SaveRequestId>{};
    }
    EditorResult<VSaveRequestStatus> MaterialEditor::Impl::saveStatus(SaveRequestId id) const
    {
        const auto* save_ = std::get_if<MaterialSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
        }
        return save_->status();
    }
    EditorResult<void> MaterialEditor::Impl::retrySave(SaveRequestId id)
    {
        if (this->busy_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.retry"});
        }

        auto* save_ = std::get_if<MaterialSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
        }
        return save_->retry(true);
    }
    EditorResult<void> MaterialEditor::Impl::abandonSave(SaveRequestId id)
    {
        if (this->busy_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.abandonSave"});
        }

        auto* save_ = std::get_if<MaterialSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
        }
        save_->abandon();
        return {};
    }
    EditorResult<void> MaterialEditor::Impl::acknowledgeSave(SaveRequestId id)
    {
        if (this->busy_ || saved_history_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.acknowledgeSave"});
        }

        auto* save_ = std::get_if<MaterialSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.save"});
        }
        if (!save_->terminal())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.save"});
        }
        this->save_.emplace<std::monostate>();
        return {};
    }

    EditorResult<lux::process::TaskId> MaterialEditor::Impl::requestCompile()
    {
        if (!history_ || saved_history_ || asset_status_.phase != EAssetEditPhase::IDLE)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.compile"});
        if (this->busy_ || compile_task_ ||
            compile_result_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.compile"});
        }
        auto view = this->history_->view();
        if (!view)
        {
            return lux::cxx::unexpected(historyFailure(view.error()));
        }
        auto assets = capturePreviewAssets();
        if (!assets)
            return lux::cxx::unexpected(assets.error());
        lux::material::MaterialSource capture{source_.id, source_.name, source_.graph.clone()};
        auto& execution = editor_context_.execution();
        auto admitted = execution.submit(
            {"Compile material", "compiler"},
            [source = std::move(capture),
             path = std::string(editor_context_.project().assetName(source_.id)),
             cpu = execution.cpu()](process::TaskReporter reporter) mutable noexcept {
                return compileMaterialAsset(std::move(source), std::move(path), cpu, reporter);
            },
            [this](process::TTaskResult<MaterialCompiled, EditorFailure>&& result) noexcept {
                compile_result_.emplace(detail::taskResult(std::move(result)));
                compile_task_ = {};
                completion_work_.request();
            }
        );
        if (!admitted)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::EXECUTION_FAILURE,
                "material.compile",
                static_cast<std::uint64_t>(admitted.error()),
                {},
                admitted.error()
            });
        const auto id = admitted->id();
        compilation_.emplace<Compilation>(
            id,
            history_->id(),
            view->snapshot.current,
            view->snapshot.revision,
            std::move(*assets)
        );
        compile_task_ = std::move(*admitted);
        return id;
    }
    EditorResult<SaveRequestId> MaterialEditor::Impl::requestPublish(lux::process::TaskId compile, std::string origin)
    {
        auto result = compiled(compile);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        if (!editor_context_.project().writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "material.publish"});
        }
        if (origin.empty() || !editor_context_.execution().blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "material.publish"});
        }
        if (this->busy_ || this->save_.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.publish"});
        }
        if (this->next_save_ == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "material.publish"});
        }
        auto target = lux::editor::detail::captureAssetSaveTarget(editor_context_.project(), this->source_.id);
        if (!target)
            return lux::cxx::unexpected(target.error());
        auto ticket = this->persistence_.capture();
        if (!ticket)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::BUSY, "asset.save.ticket", static_cast<std::uint64_t>(ticket.error())
            });
        }
        const SaveRequestId id{this->history_->id(), this->next_save_++};
        const auto& job = std::get<Compilation>(this->compilation_);
        const auto& image = job.output->publication;
        this->save_.emplace<MaterialSave>(
            id,
            *ticket,
            job.revision,
            std::move(*target),
            image,
            editor_context_.project(),
            editor_context_.execution(),
            this->persistence_,
            completion_work_.requester()
        );
        return id;
    }

    EditorResult<VMaterialCompileStatus> MaterialEditor::Impl::compileStatus(lux::process::TaskId id) const
    {
        const auto* job = std::get_if<Compilation>(&this->compilation_);
        if (!job || job->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "material.compile"});
        }
        if (const auto* success = std::get_if<MaterialCompileSucceeded>(&job->status))
        {
            auto value = *success;
            value.current = this->history_->view()->snapshot.current == value.captured;
            return VMaterialCompileStatus{value};
        }
        return job->status;
    }
    EditorResult<std::reference_wrapper<const lux::rdesc::MaterialDescription>> MaterialEditor::Impl::compiled(
        lux::process::TaskId id
    ) const
    {
        auto status = compileStatus(id);
        if (!status)
        {
            return lux::cxx::unexpected(status.error());
        }
        if (const auto* failed = std::get_if<MaterialCompileFailed>(&*status))
        {
            return lux::cxx::unexpected(failed->failure);
        }
        const auto* success = std::get_if<MaterialCompileSucceeded>(&*status);
        if (!success || !success->current)
        {
            return lux::cxx::unexpected(
                EditorFailure{success ? EEditorError::STALE_REQUEST : EEditorError::BUSY, "material.compile"}
            );
        }
        return std::cref(std::get<Compilation>(this->compilation_).output->artifact->data());
    }

    editing::HistoryId MaterialEditor::Impl::historyId() const noexcept
    {
        return history_ ? history_->id() : editing::HistoryId{};
    }
    editing::EditResult<editing::HistoryTargetView> MaterialEditor::Impl::historyView() const noexcept
    {
        if (!history_)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET));
        auto value = this->history_->view();
        if (!value)
        {
            return lux::cxx::unexpected(value.error());
        }
        using Availability = editing::EHistoryActionAvailability;
        if (this->busy_ || saved_history_ || asset_status_.phase != EAssetEditPhase::IDLE)
        {
            return editing::HistoryTargetView{value->snapshot, Availability::BUSY, Availability::BUSY, {}, {}};
        }
        return editing::HistoryTargetView{
            value->snapshot,
            value->can_undo ? Availability::READY : Availability::EMPTY,
            value->can_redo ? Availability::READY : Availability::EMPTY,
            value->undo_label,
            value->redo_label
        };
    }
    editing::EditResult<editing::HistoryTargetResult> MaterialEditor::Impl::undo() noexcept
    {
        if (auto allowed = canEdit(); !allowed)
            return lux::cxx::unexpected(allowed.error());
        BusyGuard guard(this->busy_);
        auto result = this->history_->undo();
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
    }
    editing::EditResult<editing::HistoryTargetResult> MaterialEditor::Impl::redo() noexcept
    {
        if (auto allowed = canEdit(); !allowed)
            return lux::cxx::unexpected(allowed.error());
        BusyGuard guard(this->busy_);
        auto result = this->history_->redo();
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
    }
    void MaterialEditor::Impl::event(object::EventView& event) noexcept
    {
        if (auto* request = event.getIf<FinishEditingRequest>())
        {
            event.accept();
            request->result = finishEditing();
            return;
        }
        contentCommand(event);
        if (event.accepted())
            return;
        if (auto* query = event.getIf<AssetEditorQuery>())
        {
            event.accept();
            query->matches = !query->asset.isNull() &&
                (editor_->assetId() == query->asset || editor_->assetStatus().target == query->asset);
            return;
        }

        if (lux::editor::ui::receiveCloseRequest(*editor_, event, close_request_, close_prepared_, close_decision_))
            return;
        ui::dispatchHistoryCommand(*editor_, event);
    }

    EditorResult<void> MaterialEditor::Impl::finishEditing()
    {
        if (!history_)
            return {};
        if (this->busy_ || this->finishing_interaction_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "material.finish-editing"});
        BusyGuard finishing(this->finishing_interaction_);
        return finishContentEditing();
    }

    void MaterialEditor::Impl::adoptCompletions() noexcept
    {
        if (busy_)
            return;
        completion_deferred_ = false;
        const bool preview_busy = preview_ && preview_->scene &&
                                  !preview_->runtime.getSceneRegistry(*preview_->scene);
        if (preview_busy && std::holds_alternative<Compilation>(compilation_))
            completion_deferred_ = true;
        if (auto* job = std::get_if<Compilation>(&this->compilation_); job && compile_result_ && !preview_busy)
        {
            auto result = std::move(*compile_result_);
            compile_result_.reset();
            if (result)
            {
                if (history_ && asset_status_.phase == EAssetEditPhase::IDLE &&
                    job->history == this->history_->id() && this->history_->view()->snapshot.current == job->state)
                    updatePreview(result->publication.artifact.source_bytes, job->state, job->preview_assets);
                job->output.emplace(std::move(*result));
                job->status = MaterialCompileSucceeded{
                    job->state,
                    job->revision,
                    this->history_->view()->snapshot.current == job->state
                };
            }
            else
            {
                job->status = MaterialCompileFailed{job->state, job->revision, std::move(result.error())};
            }
            // Callbacks may request close; physical removal is an Editor safe-point operation.
            const auto completed_id = job->id;
            BusyGuard guard(this->busy_);
            lux::editor::detail::reportSignalDelivery(
                editor_->emit(editor_->compileFinished, completed_id),
                "compileFinished"
            );
        }
        adoptAssetResults();
        ui::reportCloseDecision(*editor_, close_request_, close_prepared_, close_decision_);
    }

    void MaterialEditor::Impl::update() noexcept
    {
        maintainPreview();
        if (this->busy_)
        {
            return;
        }
        const auto title = source_.id.isNull() ? std::string("Material Editor") : source_.name;
        if (editor_->title() != title)
            editor_->setTitle(title);
        if (std::exchange(hide_requested_, false))
        {
            PaneCloseRequest request{editor_};
            static_cast<void>(object::routeEvent(*editor_, editor_->root(), request));
        }
        if (history_ &&
            asset_status_.phase == EAssetEditPhase::IDLE && !saved_history_)
            applyContentIntents();
        const bool has_pending_change = completion_deferred_ || asset_status_.phase != EAssetEditPhase::IDLE;
        if (has_pending_change)
            editor_->root().deferChange(*editor_, [](object::LuxObject& target) noexcept {
                static_cast<MaterialEditor&>(target).impl_->applyChanges();
            });
    }

} // namespace lux::editor::material

namespace lux::editor::material
{
    EditorResult<SaveRequestId> MaterialEditor::requestSave(std::string origin)
    {
        return impl_->requestSave(std::move(origin));
    }

    std::span<const SaveRequestId> MaterialEditor::saveRequests() const noexcept
    {
        return impl_->saveRequests();
    }

    EditorResult<VSaveRequestStatus> MaterialEditor::saveStatus(SaveRequestId id) const
    {
        return impl_->saveStatus(std::move(id));
    }

    EditorResult<void> MaterialEditor::retrySave(SaveRequestId id)
    {
        return impl_->retrySave(std::move(id));
    }

    EditorResult<void> MaterialEditor::abandonSave(SaveRequestId id)
    {
        return impl_->abandonSave(std::move(id));
    }

    EditorResult<void> MaterialEditor::acknowledgeSave(SaveRequestId id)
    {
        return impl_->acknowledgeSave(std::move(id));
    }

    EditorResult<lux::process::TaskId> MaterialEditor::requestCompile()
    {
        return impl_->requestCompile();
    }

    EditorResult<SaveRequestId> MaterialEditor::requestPublish(lux::process::TaskId compile, std::string origin)
    {
        return impl_->requestPublish(std::move(compile), std::move(origin));
    }

    EditorResult<VMaterialCompileStatus> MaterialEditor::compileStatus(lux::process::TaskId id) const
    {
        return impl_->compileStatus(std::move(id));
    }

    EditorResult<std::reference_wrapper<const lux::rdesc::MaterialDescription>> MaterialEditor::compiled(
        lux::process::TaskId id
    ) const
    {
        return impl_->compiled(std::move(id));
    }

    editing::HistoryId MaterialEditor::historyId() const noexcept
    {
        return impl_->historyId();
    }

    editing::EditResult<editing::HistoryTargetView> MaterialEditor::historyView() const noexcept
    {
        return impl_->historyView();
    }

    editing::EditResult<editing::HistoryTargetResult> MaterialEditor::undo() noexcept
    {
        return impl_->undo();
    }

    editing::EditResult<editing::HistoryTargetResult> MaterialEditor::redo() noexcept
    {
        return impl_->redo();
    }

    void MaterialEditor::event(object::EventView& event) noexcept
    {
        return impl_->event(event);
    }

    EditorResult<void> MaterialEditor::finishEditing()
    {
        return impl_->finishEditing();
    }

    void MaterialEditor::update() noexcept
    {
        impl_->update();
        lux::editor::ui::reportCloseDecision(
            *this,
            impl_->close_request_,
            impl_->close_prepared_,
            impl_->close_decision_
        );
    }
}

namespace lux::editor::material
{
    void MaterialEditor::Impl::applyChanges() noexcept
    {
        if (busy_)
            return;
        adoptCompletions();
        applyAssetChange();
    }
    bool MaterialEditor::hasUnsavedChanges() const noexcept
    {
        return impl_->history_ && !impl_->persistence_.clean();
    }
    std::optional<sessions::PersistedState> MaterialEditor::persistedState() const noexcept
    {
        return impl_->history_ ? impl_->persistence_.persisted() : std::nullopt;
    }
}
