#include <lux/engine/editor/metadata/AssetEditorRegistration.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditorImpl.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/ui/HistoryCommands.hpp>
#include <algorithm>
#include <cmath>
#include <lux/engine/editor/detail/AssetSave.hpp>
#include <lux/engine/editor/metadata/EditorReflection.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/editor/flowforge/FlowCompilation.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <unordered_map>
#include <unordered_set>

namespace lux::editor::flowforge
{
    namespace
    {
        constexpr editing::HistoryLimits kLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};
        EditorFailure historyFailure(const editing::EditFailure& failure)
        {
            return {
                EEditorError::INVALID_STATE,
                "flowforge.history",
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

    FlowEditObserver FlowForgeEditor::Impl::editObserver() noexcept
    {
        return {this, [](void* raw, const editing::CommitInfo& info, bool structural) noexcept {
                    auto& owner = *static_cast<Impl*>(raw);
                    if (structural)
                        owner.indexContent();
                    lux::editor::detail::reportSignalDelivery(
                        owner.editor_->emit(owner.editor_->contentChanged, info.revision),
                        "FlowForgeEditor::contentChanged"
                    );
                }};
    }
    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::applyEdit(VFlowEdit edit)
    {
        if (auto allowed = canEdit(); !allowed)
            return lux::cxx::unexpected(allowed.error());
        BusyGuard guard(busy_);
        std::vector<VFlowEdit> edits;
        edits.push_back(std::move(edit));
        auto code = environment_.code_lifetime ? contracts::CodeLease::plugin(environment_.code_lifetime)
                                               : contracts::CodeLease::builtin();
        auto prepared = prepareFlowEdit(
            source_,
            environment_,
            history_->view()->snapshot.current,
            std::move(edits),
            {},
            std::move(code),
            editObserver()
        );
        if (!prepared)
            return lux::cxx::unexpected(prepared.error());
        return history_->execute(prepared->operation);
    }
    editing::EditResult<FlowEditIds> FlowForgeEditor::Impl::insertEdit(VFlowEdit edit)
    {
        if (auto allowed = canEdit(); !allowed)
            return lux::cxx::unexpected(allowed.error());
        BusyGuard guard(busy_);
        std::vector<VFlowEdit> edits;
        edits.push_back(std::move(edit));
        auto code = environment_.code_lifetime ? contracts::CodeLease::plugin(environment_.code_lifetime)
                                               : contracts::CodeLease::builtin();
        auto prepared = prepareFlowEdit(
            source_,
            environment_,
            history_->view()->snapshot.current,
            std::move(edits),
            {},
            std::move(code),
            editObserver()
        );
        if (!prepared)
            return lux::cxx::unexpected(prepared.error());
        auto result = history_->execute(prepared->operation);
        if (!result)
            return lux::cxx::unexpected(result.error());
        return *prepared->inserted;
    }
    void FlowForgeEditor::Impl::indexContent()
    {
        read_nodes_.clear();
        read_pins_.clear();
        read_nodes_.reserve(source_.graph.nodes().size());
        read_pins_.reserve(source_.graph.topology().pins().size());
        for (const auto& storage : source_.graph.nodes())
        {
            const auto* node = storage.node.get();
            read_nodes_.emplace(node->id(), node);
            for (const auto* pin : node->inPins())
            {
                read_pins_.emplace(pin->id(), pin);
            }
            for (const auto* pin : node->outPins())
            {
                read_pins_.emplace(pin->id(), pin);
            }
        }
    }

    editing::EditResult<void> FlowForgeEditor::Impl::canEdit() const noexcept
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

    FlowForgeEditor::FlowForgeEditor(
        lux::ui::Root& parent,
        lux::ui::PaneId id,
        std::unique_ptr<Impl> data,
        EditorResult<void>& status
    )
        : lux::ui::Pane(parent, std::move(id), lux::ui::PaneTypeId{kFlowForgeEditorType}, "FlowForge Editor"),
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
    FlowForgeEditor::~FlowForgeEditor() = default;

    EditorResult<std::unique_ptr<FlowForgeEditor>> FlowForgeEditor::create(
        lux::ui::Root& parent,
        lux::ui::PaneId id,
        EditorContext& context
    ) noexcept
    try
    {
        struct Selection final
        {
            std::shared_ptr<const void> code{acquireEditorReflection()};
            std::vector<const lux::meta::RefClass*> classes;
            std::vector<const lux::meta::RefFunction*> functions;
        };
        auto selected = std::make_shared<Selection>();
        const auto& registry = lux::meta::ReflectionRegistry::instance();
        for (const auto& type : registry.classes())
            if (type && type->type.size)
                selected->classes.push_back(type.get());
        for (const auto& function : registry.functions())
            if (function)
                selected->functions.push_back(function.get());
        auto data = std::make_unique<Impl>(context);
        data->environment_.classes = selected->classes;
        data->environment_.functions = selected->functions;
        data->environment_.code_lifetime = selected;
        auto valid = lux::flowforge::validateFlowSourceEnvironment(data->environment_);
        if (!valid)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.metadata", 0, {}, valid.error()}
            );
        EditorResult<void> status;
        auto result =
            std::unique_ptr<FlowForgeEditor>(new FlowForgeEditor(parent, std::move(id), std::move(data), status));
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
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "flowforge.create"});
    }

    EditorResult<lux::flowforge::FlowSource> FlowForgeEditor::Impl::capture() const
    {
        auto result = lux::flowforge::captureFlowSource(this->source_.id, this->source_.name, this->source_.graph);
        if (!result)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "flowforge.capture",
                static_cast<std::uint64_t>(result.error().code),
                result.error().field,
                result.error()
            });
        }
        return std::move(*result);
    }
    ProjectStorage& FlowForgeEditor::Impl::project() noexcept
    {
        return editor_context_.project();
    }
    const lux::flowforge::FlowSourceEnvironment& FlowForgeEditor::Impl::metadata() const noexcept
    {
        return this->environment_;
    }
    std::span<const lux::graph::NodeRecord> FlowForgeEditor::Impl::nodes() const noexcept
    {
        return this->source_.graph.topology().nodes();
    }
    std::span<const lux::graph::PinRecord> FlowForgeEditor::Impl::pins() const noexcept
    {
        return this->source_.graph.topology().pins();
    }
    std::span<const lux::graph::LinkRecord> FlowForgeEditor::Impl::links() const noexcept
    {
        return this->source_.graph.topology().links();
    }
    std::string_view FlowForgeEditor::Impl::nodeName(lux::flowforge::NodeId id) const noexcept
    {
        const auto found = this->read_nodes_.find(id);
        const auto* node = found == this->read_nodes_.end() ? nullptr : found->second;
        return node ? std::string_view(node->name()) : std::string_view{};
    }
    lux::flowforge::ENodeOperation FlowForgeEditor::Impl::nodeOperation(lux::flowforge::NodeId id) const noexcept
    {
        const auto found = this->read_nodes_.find(id);
        const auto* node = found == this->read_nodes_.end() ? nullptr : found->second;
        return node ? node->operation() : lux::flowforge::ENodeOperation::INVALID;
    }
    std::string_view FlowForgeEditor::Impl::pinName(lux::flowforge::PinId id) const noexcept
    {
        const auto found = this->read_pins_.find(id);
        const auto* pin = found == this->read_pins_.end() ? nullptr : found->second;
        return pin ? std::string_view(pin->name()) : std::string_view{};
    }
    std::string_view FlowForgeEditor::Impl::pinType(lux::flowforge::PinId id) const noexcept
    {
        const auto found = this->read_pins_.find(id);
        const auto* pin = found == this->read_pins_.end() ? nullptr : found->second;
        if (pin && pin->kind() == lux::flowforge::EPinKind::DATA_IN)
        {
            return static_cast<const lux::flowforge::DataInPin*>(pin)->info().type->name;
        }
        if (pin && pin->kind() == lux::flowforge::EPinKind::DATA_OUT)
        {
            return static_cast<const lux::flowforge::DataOutPin*>(pin)->info().type->name;
        }
        return {};
    }
    lux::graph::GraphNodeLayout FlowForgeEditor::Impl::nodeLayout(lux::flowforge::NodeId id) const noexcept
    {
        const auto* layout = this->source_.graph.layout().find(id);
        return layout ? *layout : lux::graph::GraphNodeLayout{};
    }

    EditorResult<lux::flowforge::FlowSourceLiteral> FlowForgeEditor::Impl::pinLiteral(lux::flowforge::PinId id) const
    {
        const auto found = this->read_pins_.find(id);
        if (found == this->read_pins_.end() || found->second->kind() != lux::flowforge::EPinKind::DATA_IN)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.literal"});
        }
        const auto& pin = *static_cast<const lux::flowforge::DataInPin*>(found->second);
        auto result = lux::flowforge::captureFlowLiteral(pin.constantData());
        if (!result)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "flowforge.literal",
                static_cast<std::uint64_t>(result.error().code),
                result.error().field,
                result.error()
            });
        }
        return *result;
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::setPinLiteral(
        lux::flowforge::PinId id,
        const lux::flowforge::FlowSourceLiteral& literal
    )
    {
        return applyEdit(FlowSetLiteral{id, literal});
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::rename(std::string_view name)
    {
        return applyEdit(FlowRename{std::string(name)});
    }
    editing::EditResult<lux::flowforge::NodeId> FlowForgeEditor::Impl::insertNode(
        std::unique_ptr<lux::flowforge::Node>& input,
        lux::graph::GraphNodeLayout placement
    )
    {
        if (auto allowed = canEdit(); !allowed)
            return lux::cxx::unexpected(allowed.error());
        BusyGuard guard(busy_);
        auto prepared = prepareBorrowedFlowInsert(
            source_,
            environment_,
            history_->view()->snapshot.current,
            input,
            placement,
            editObserver()
        );
        if (!prepared)
            return lux::cxx::unexpected(prepared.error());
        auto result = history_->execute(prepared->operation);
        if (!result)
            return lux::cxx::unexpected(result.error());
        return prepared->inserted->nodes.front();
    }
    EditorResult<lux::flowforge::FlowSourceNode> FlowForgeEditor::Impl::captureNode(lux::flowforge::NodeId id) const
    {
        const auto found = this->read_nodes_.find(id);
        if (found == this->read_nodes_.end())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.node"});
        }
        auto value = lux::flowforge::captureFlowNode(*found->second);
        if (!value)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "flowforge.node",
                static_cast<std::uint64_t>(value.error().code),
                {},
                value.error()
            });
        }
        value->layout = nodeLayout(id);
        return std::move(*value);
    }

    editing::EditResult<lux::flowforge::NodeId> FlowForgeEditor::Impl::insertFunctionUse(
        lux::flowforge::NodeId id,
        bool return_node,
        lux::graph::GraphNodeLayout layout
    )
    {
        auto result = insertEdit(FlowInsertFunctionUse{id, return_node, layout});
        if (!result)
            return lux::cxx::unexpected(result.error());
        return result->nodes.front();
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::setFunctionSignature(
        editing::StateId base,
        lux::flowforge::NodeId id,
        std::string_view name,
        const lux::flowforge::FlowSourceSignature& signature
    )
    {
        if (base != history_->view()->snapshot.current)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_BASE));
        return applyEdit(FlowSetSignature{id, std::string(name), signature});
    }

    std::span<const lux::flowforge::ExportMethodNode> FlowForgeEditor::Impl::exports() const noexcept
    {
        return this->source_.graph.exports();
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::setExports(
        std::vector<lux::flowforge::ExportMethodNode> exports
    )
    {
        return applyEdit(FlowSetExports{std::move(exports)});
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::removeNodes(
        std::span<const lux::flowforge::NodeId> nodes,
        std::span<const lux::graph::LinkRecord> links
    )
    {
        return applyEdit(FlowRemoveNodes{{nodes.begin(), nodes.end()}, {links.begin(), links.end()}});
    }
    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::connect(
        lux::flowforge::PinId from,
        lux::flowforge::PinId to
    )
    {
        return applyEdit(FlowConnect{from, to});
    }
    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::disconnect(
        lux::flowforge::PinId from,
        lux::flowforge::PinId to
    )
    {
        return applyEdit(FlowDisconnect{from, to});
    }
    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::moveNodes(
        std::span<const lux::graph::GraphLayoutEntry> entries
    )
    {
        return applyEdit(FlowMoveNodes{{entries.begin(), entries.end()}});
    }

    std::span<const lux::flowforge::FlowGraph::GraphVariable> FlowForgeEditor::Impl::variables() const noexcept
    {
        return this->source_.graph.variables();
    }

    editing::EditResult<std::uint64_t> FlowForgeEditor::Impl::addVariable(
        std::string_view name,
        std::string_view type,
        const lux::flowforge::FlowSourceLiteral& initial
    )
    {
        auto result = insertEdit(FlowAddVariable{std::string(name), std::string(type), initial});
        if (!result)
            return lux::cxx::unexpected(result.error());
        return result->variables.front();
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::setVariable(
        const lux::flowforge::FlowSourceVariable& value
    )
    {
        return applyEdit(FlowSetVariable{value});
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::removeVariable(std::uint64_t id)
    {
        return applyEdit(FlowRemoveVariable{id});
    }

    EditorResult<SaveRequestId> FlowForgeEditor::Impl::requestSave(std::string origin)
    {
        if (!history_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "flowforge.empty"});
        if (asset_status_.phase != EAssetEditPhase::IDLE && asset_status_.phase != EAssetEditPhase::REVIEW)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.save"});
        if (!editor_context_.project().writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "flowforge.save"});
        }
        if (origin.empty() || !editor_context_.execution().blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.save"});
        }
        if (this->busy_ || this->save_.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.save"});
        }
        if (this->next_save_ == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "flowforge.save"});
        }
        auto capture = this->capture();
        if (!capture)
        {
            return lux::cxx::unexpected(capture.error());
        }
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
        this->save_.emplace<FlowSave>(
            id,
            *ticket,
            this->history_->view()->snapshot.revision,
            std::move(*target),
            std::move(*capture),
            editor_context_.project(),
            editor_context_.execution(),
            this->persistence_,
            completion_work_.requester()
        );
        return id;
    }
    std::span<const SaveRequestId> FlowForgeEditor::Impl::saveRequests() const noexcept
    {
        const auto* save_ = std::get_if<FlowSave>(&this->save_);
        return save_ ? save_->requests() : std::span<const SaveRequestId>{};
    }
    EditorResult<VSaveRequestStatus> FlowForgeEditor::Impl::saveStatus(SaveRequestId id) const
    {
        const auto* save_ = std::get_if<FlowSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        return save_->status();
    }
    EditorResult<void> FlowForgeEditor::Impl::retrySave(SaveRequestId id)
    {
        if (this->busy_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.retry"});
        }

        auto* save_ = std::get_if<FlowSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        return save_->retry(true);
    }
    EditorResult<void> FlowForgeEditor::Impl::abandonSave(SaveRequestId id)
    {
        if (this->busy_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.abandonSave"});
        }

        auto* save_ = std::get_if<FlowSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        save_->abandon();
        return {};
    }
    EditorResult<void> FlowForgeEditor::Impl::acknowledgeSave(SaveRequestId id)
    {
        if (this->busy_ || saved_history_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.acknowledgeSave"});
        }

        auto* save_ = std::get_if<FlowSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        if (!save_->terminal())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.save"});
        }
        this->save_.emplace<std::monostate>();
        return {};
    }

    EditorResult<lux::process::TaskId> FlowForgeEditor::Impl::requestCompile(std::filesystem::path linker)
    {
        if (!history_ || saved_history_ || asset_status_.phase != EAssetEditPhase::IDLE)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.compile"});
        if (!editor_context_.execution().blocking() || this->busy_ || compile_task_ || compile_result_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.compile"});
        }
        auto view = this->history_->view();
        if (!view)
        {
            return lux::cxx::unexpected(historyFailure(view.error()));
        }
        auto capture = this->capture();
        if (!capture)
        {
            return lux::cxx::unexpected(capture.error());
        }
        auto& execution = editor_context_.execution();
        auto admitted = execution.submit(
            {"Compile Flow", "compiler", {}, environment_.code_lifetime},
            [source_ = std::move(*capture),
             environment_ = environment_,
             path = std::string(editor_context_.project().assetName(source_.id)),
             linker = std::move(linker),
             cpu = execution.cpu(),
             blocking = *execution.blocking()](process::TaskReporter reporter) mutable noexcept {
                return compileFlowAsset(
                    std::move(source_),
                    environment_,
                    std::move(path),
                    std::move(linker),
                    cpu,
                    blocking,
                    reporter
                );
            },
            [this](process::TTaskResult<FlowCompiled, FlowCompilationFailure>&& result) noexcept {
                acceptCompilation(std::move(result));
            }
        );
        if (!admitted)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::EXECUTION_FAILURE,
                "flowforge.compile",
                static_cast<std::uint64_t>(admitted.error()),
                {},
                admitted.error()
            });
        const auto id = admitted->id();
        compilation_.emplace<Compilation>(id, history_->id(), view->snapshot.current, view->snapshot.revision);
        compile_task_ = std::move(*admitted);
        return id;
    }
    EditorResult<SaveRequestId> FlowForgeEditor::Impl::requestPublish(lux::process::TaskId compile, std::string origin)
    {
        auto result = compiled(compile);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        if (!editor_context_.project().writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "flowforge.publish"});
        }
        if (origin.empty() || !editor_context_.execution().blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.publish"});
        }
        if (this->busy_ || this->save_.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.publish"});
        }
        if (this->next_save_ == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "flowforge.publish"});
        }
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
        const auto& job = std::get<Compilation>(this->compilation_);
        const auto& image = job.output->publication;
        this->save_.emplace<FlowSave>(
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

    EditorResult<VFlowCompileStatus> FlowForgeEditor::Impl::compileStatus(lux::process::TaskId id) const
    {
        const auto* job = std::get_if<Compilation>(&this->compilation_);
        if (!job || job->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.compile"});
        }
        if (const auto* success = std::get_if<FlowCompileSucceeded>(&job->status))
        {
            auto value = *success;
            value.current = this->history_->view()->snapshot.current == value.captured;
            return VFlowCompileStatus{value};
        }
        return job->status;
    }
    EditorResult<std::reference_wrapper<const lux::script::ScriptArtifact>> FlowForgeEditor::Impl::compiled(
        lux::process::TaskId id
    ) const
    {
        auto status = compileStatus(id);
        if (!status)
        {
            return lux::cxx::unexpected(status.error());
        }
        if (const auto* failed = std::get_if<FlowCompileFailed>(&*status))
        {
            return lux::cxx::unexpected(failed->failure);
        }
        const auto* success = std::get_if<FlowCompileSucceeded>(&*status);
        if (!success || !success->current)
        {
            return lux::cxx::unexpected(
                EditorFailure{success ? EEditorError::STALE_REQUEST : EEditorError::BUSY, "flowforge.compile"}
            );
        }
        return std::cref(std::get<Compilation>(this->compilation_).output->artifact->data());
    }

    EditorResult<void> FlowForgeEditor::Impl::retryLink(lux::process::TaskId id, std::filesystem::path linker)
    {
        auto* job = std::get_if<Compilation>(&this->compilation_);
        if (!job || job->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.link.retry"});
        }
        const auto* failure = std::get_if<FlowCompileFailed>(&job->status);
        if (!failure || !failure->retryable || this->busy_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.link.retry"});
        }
        auto& execution = editor_context_.execution();
        auto admitted = execution.submit(
            {"Retry Flow link", "compiler", job->id, environment_.code_lifetime},
            [&, linker = std::move(linker)](process::TaskReporter reporter) mutable noexcept {
                return linkFlowAsset(
                    std::move(*job->retry),
                    std::move(linker),
                    execution.cpu(),
                    *execution.blocking(),
                    reporter
                );
            },
            [this](process::TTaskResult<FlowCompiled, FlowCompilationFailure>&& result) noexcept {
                acceptCompilation(std::move(result));
            }
        );
        if (!admitted)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::EXECUTION_FAILURE,
                "flowforge.link.retry",
                static_cast<std::uint64_t>(admitted.error()),
                {},
                admitted.error()
            });
        job->retry.reset();
        job->status = FlowCompilePending{EFlowCompileStage::LINKING};
        compile_task_ = std::move(*admitted);
        return {};
    }

    editing::HistoryId FlowForgeEditor::Impl::historyId() const noexcept
    {
        return history_ ? history_->id() : editing::HistoryId{};
    }
    editing::EditResult<editing::HistoryTargetView> FlowForgeEditor::Impl::historyView() const noexcept
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
    editing::EditResult<editing::HistoryTargetResult> FlowForgeEditor::Impl::undo() noexcept
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
    editing::EditResult<editing::HistoryTargetResult> FlowForgeEditor::Impl::redo() noexcept
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
    void FlowForgeEditor::Impl::event(object::EventView& event) noexcept
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

    EditorResult<void> FlowForgeEditor::Impl::finishEditing()
    {
        if (!history_)
            return {};
        if (this->busy_ || this->finishing_interaction_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flow.finish-editing"});
        BusyGuard finishing(this->finishing_interaction_);
        return finishContentEditing();
    }

    void FlowForgeEditor::Impl::acceptCompilation(process::TTaskResult<FlowCompiled, FlowCompilationFailure>&& result
    ) noexcept
    {
        if (result)
            compile_result_.emplace(std::move(*result));
        else if (auto* domain_failure = result.error().domainFailure())
            compile_result_.emplace(lux::cxx::unexpected(std::move(*domain_failure)));
        else
        {
            EditorFailure failure{EEditorError::CANCELLED, "flowforge.compile"};
            if (const auto* error = result.error().executionFailure())
                failure = {
                    EEditorError::EXECUTION_FAILURE,
                    "flowforge.compile",
                    static_cast<std::uint64_t>(*error),
                    {},
                    *error
                };
            compile_result_.emplace(lux::cxx::unexpected(FlowCompilationFailure{std::move(failure)}));
        }
        compile_task_ = {};
        completion_work_.request();
    }

    void FlowForgeEditor::Impl::adoptCompletions() noexcept
    {
        if (busy_)
            return;
        completion_pending_ = false;
        if (auto* job = std::get_if<Compilation>(&this->compilation_); job && compile_result_)
        {
            auto result = std::move(*compile_result_);
            compile_result_.reset();
            if (result)
            {
                job->output.emplace(std::move(*result));
                job->status = FlowCompileSucceeded{job->state, job->revision, true};
            }
            else
            {
                auto error = std::move(result.error());
                job->retry = std::move(error.retry);
                job->status =
                    FlowCompileFailed{job->state, job->revision, std::move(error.failure), job->retry.has_value()};
            }
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
    void FlowForgeEditor::Impl::update() noexcept
    {
        if (this->busy_)
        {
            return;
        }
        const auto title = source_.id.isNull() ? std::string("FlowForge Editor") : source_.name;
        if (editor_->title() != title)
            editor_->setTitle(title);
        if (std::exchange(hide_requested_, false))
        {
            PaneCloseRequest request{editor_};
            static_cast<void>(object::routeEvent(*editor_, editor_->root(), request));
        }
        if (history_ && asset_status_.phase == EAssetEditPhase::IDLE && !saved_history_)
            applyContentIntents();
        const bool has_pending_change = completion_pending_ || asset_status_.phase != EAssetEditPhase::IDLE;
        if (has_pending_change)
            editor_->root().deferChange(*editor_, [](object::LuxObject& target) noexcept {
                static_cast<FlowForgeEditor&>(target).impl_->applyChanges();
            });
    }

} // namespace lux::editor::flowforge

namespace lux::editor::flowforge
{
    EditorResult<SaveRequestId> FlowForgeEditor::requestSave(std::string origin)
    {
        return impl_->requestSave(std::move(origin));
    }

    std::span<const SaveRequestId> FlowForgeEditor::saveRequests() const noexcept
    {
        return impl_->saveRequests();
    }

    EditorResult<VSaveRequestStatus> FlowForgeEditor::saveStatus(SaveRequestId id) const
    {
        return impl_->saveStatus(std::move(id));
    }

    EditorResult<void> FlowForgeEditor::retrySave(SaveRequestId id)
    {
        return impl_->retrySave(std::move(id));
    }

    EditorResult<void> FlowForgeEditor::abandonSave(SaveRequestId id)
    {
        return impl_->abandonSave(std::move(id));
    }

    EditorResult<void> FlowForgeEditor::acknowledgeSave(SaveRequestId id)
    {
        return impl_->acknowledgeSave(std::move(id));
    }

    EditorResult<lux::process::TaskId> FlowForgeEditor::requestCompile(std::filesystem::path linker)
    {
        return impl_->requestCompile(std::move(linker));
    }

    EditorResult<SaveRequestId> FlowForgeEditor::requestPublish(lux::process::TaskId compile, std::string origin)
    {
        return impl_->requestPublish(std::move(compile), std::move(origin));
    }

    EditorResult<VFlowCompileStatus> FlowForgeEditor::compileStatus(lux::process::TaskId id) const
    {
        return impl_->compileStatus(std::move(id));
    }

    EditorResult<std::reference_wrapper<const lux::script::ScriptArtifact>> FlowForgeEditor::compiled(
        lux::process::TaskId id
    ) const
    {
        return impl_->compiled(std::move(id));
    }

    EditorResult<void> FlowForgeEditor::retryLink(lux::process::TaskId id, std::filesystem::path linker)
    {
        return impl_->retryLink(std::move(id), std::move(linker));
    }

    editing::HistoryId FlowForgeEditor::historyId() const noexcept
    {
        return impl_->historyId();
    }

    editing::EditResult<editing::HistoryTargetView> FlowForgeEditor::historyView() const noexcept
    {
        return impl_->historyView();
    }

    editing::EditResult<editing::HistoryTargetResult> FlowForgeEditor::undo() noexcept
    {
        return impl_->undo();
    }

    editing::EditResult<editing::HistoryTargetResult> FlowForgeEditor::redo() noexcept
    {
        return impl_->redo();
    }

    void FlowForgeEditor::event(object::EventView& event) noexcept
    {
        return impl_->event(event);
    }

    EditorResult<void> FlowForgeEditor::finishEditing()
    {
        return impl_->finishEditing();
    }

    void FlowForgeEditor::update() noexcept
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

namespace lux::editor::flowforge
{
    void FlowForgeEditor::Impl::applyChanges() noexcept
    {
        if (busy_)
            return;
        adoptCompletions();
        applyAssetChange();
    }
    bool FlowForgeEditor::hasUnsavedChanges() const noexcept
    {
        return impl_->history_ && !impl_->persistence_.clean();
    }
    std::optional<sessions::PersistedState> FlowForgeEditor::persistedState() const noexcept
    {
        return impl_->history_ ? impl_->persistence_.persisted() : std::nullopt;
    }
}
