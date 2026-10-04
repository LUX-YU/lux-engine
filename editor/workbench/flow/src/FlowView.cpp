#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>
#include <lux/engine/editor/flowforge/PublishFlowArtifact.hpp>
#include <lux/engine/editor/workbench/InteractionDelivery.hpp>
#include <lux/engine/editor/flowforge/FlowView.hpp>
#include <lux/engine/editor/workbench/ViewPreparation.hpp>
#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/editor/flowforge/FlowNodeControls.hpp>
#include <lux/engine/editor/widgets/GraphCanvas.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <deque>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>

namespace lux::editor::flowforge
{
    namespace
    {
        constexpr sessions::SessionKindIdView kContentKinds[]{sessions::SessionKindIdView{"lux.editor.flowforge"}};
        constexpr views::ViewFactoryDescriptor kViewDescriptor{
            views::ViewTypeIdView{"lux.editor.flowforge"},
            "FlowForge",
            cxx::typeToken<views::ContentViewInput>(),
            1,
            kContentKinds
        };
        template <class T> auto rejected(T error)
        {
            return cxx::unexpected(VFlowViewFailure{std::move(error)});
        }
        template <class T> FlowViewResult<void> accepted(T result)
        {
            if (!result)
                return rejected(result.error());
            return {};
        }
        bool temporary(const VFlowViewFailure& failure)
        {
            return std::visit(
                [](const auto& error)
                {
                    using T = std::decay_t<decltype(error)>;
                    if constexpr (std::same_as<T, FlowEditError>)
                        return error.code == EFlowEditError::SESSION && error.session == sessions::ESessionError::BUSY;
                    else if constexpr (std::same_as<T, views::EViewError>)
                        return error == views::EViewError::BUSY;
                    else if constexpr (std::same_as<T, persistence::PersistenceFailure>)
                        return error.code == persistence::EPersistenceError::BUSY;
                    else if constexpr (std::same_as<T, VFlowCompilationFailure>)
                    {
                        const auto* code = std::get_if<EFlowCompilationError>(&error);
                        return code && *code == EFlowCompilationError::BUSY;
                    }
                    else
                        return false;
                },
                failure
            );
        }
        struct Display final
        {
            sessions::ContentStamp content;
            std::string name;
            std::vector<widgets::CanvasNode> nodes;
            std::vector<widgets::CanvasLink> links;
            std::vector<lux::flowforge::FlowSourceVariable> variables;
            std::vector<lux::flowforge::ExportMethodNode> exports;
        };
        FlowViewResult<Display> display(const FlowSession& session)
        {
            const auto content = session.describe().current;
            auto read = session.read();
            if (!read)
                return rejected(read.error());
            auto current = read->withRead(
                [&](const lux::flowforge::FlowSource& source) -> FlowEditResult<Display>
                {
                    Display value;
                    value.content = content;
                    value.name = source.name;
                    value.variables = source.variables;
                    for (const auto& node : source.nodes)
                    {
                        widgets::CanvasNode
                            row{node.id.value, node.name, {}, {node.layout.x, node.layout.y}, node.layout.placed};
                        for (const auto& pin : node.inputs)
                            row.pins.push_back({pin.id.value, pin.name, true});
                        for (const auto& pin : node.outputs)
                            row.pins.push_back({pin.id.value, pin.name, false});
                        value.nodes.push_back(std::move(row));
                    }
                    for (const auto& link : source.links)
                        value.links.push_back({link.from.value, link.to.value});
                    for (const auto& entry : source.exports)
                        value.exports.push_back(
                            {lux::flowforge::FlowForgeExportNodeId{entry.id}, entry.entry, entry.symbol, entry.hints}
                        );
                    return value;
                }
            );
            if (!current)
                return rejected(current.error());
            return std::move(*current);
        }
    } // namespace
    struct FlowView::Impl final
    {
        enum class EControl : std::uint8_t
        {
            NONE,
            UNDO,
            REDO,
            COMPILE,
            RETRY,
            PUBLISH,
            REVERT,
            CANCEL
        };
        FlowView& view_;
        FlowViewServices services_;
        std::unique_ptr<FlowInteraction> interaction_;
        FlowViewState state_;
        std::optional<FlowViewBinding> binding_;
        FlowViewResult<void> status_;
        Display display_;
        struct NodePropertiesDraft final
        {
            sessions::ContentStamp based_on;
            lux::flowforge::FlowSourceNode value;
        };
        std::optional<NodePropertiesDraft> properties_;
        std::optional<std::uint64_t> selected_;
        using ECanvasStage = workbench::detail::EInputDeliveryStage;
        struct CanvasRequest final
        {
            sessions::ContentStamp based_on;
            widgets::CanvasEdit input;
            std::vector<VFlowEdit> edits;
            ECanvasStage stage;
        };
        std::deque<CanvasRequest> canvas_edit_;
        EControl control_{};
        FlowCompileId compile_;
        std::string compile_status_;
        lux::ui::Layout layout_;
        widgets::GraphCanvas graph_;
        template <class MakeEdit> FlowViewResult<void> enqueue(sessions::ContentStamp based_on, MakeEdit make_edit)
        {
            if (canvas_edit_.size() >= 64)
                return rejected(views::EViewError::CAPACITY);
            canvas_edit_.push_back({based_on, widgets::CanvasEdit{{}, true, true, false}, {}, ECanvasStage::BEGIN});
            canvas_edit_.back().edits.emplace_back(make_edit());
            return {};
        }
        FlowViewResult<void> validate(sessions::ContentStamp based_on) const
        {
            auto info = services_.sessions.describe(binding_->session);
            if (!info)
                return rejected(FlowEditError{info.error()});
            if (info->admission != sessions::EEditAdmission::AVAILABLE)
                return rejected(FlowEditError{sessions::ESessionError::BUSY});
            if (info->current != based_on)
                return rejected(FlowEditError{EFlowEditError::STALE_CONTENT});
            return {};
        }
        FlowViewResult<void> rejectInput(const VFlowViewFailure& failure)
        {
            if (temporary(failure))
                return cxx::unexpected(failure);
            const auto based_on = canvas_edit_.front().based_on;
            const auto* overlay = binding_->interaction->overlay();
            if (overlay && overlay->expected == based_on)
            {
                auto cancelled = binding_->interaction->cancel();
                if (!cancelled)
                    return rejected(cancelled.error());
            }
            const auto clear = [&]
            {
                canvas_edit_.pop_front();
                graph_.setEnabled(canvas_edit_.size() < 60);
            };
            auto owner = services_.sessions.read(binding_->session);
            if (!owner)
            {
                if (owner.error() != sessions::ESessionError::STALE_SESSION)
                    return rejected(FlowEditError{owner.error()});
                clear();
                return cxx::unexpected(failure);
            }
            auto read = owner->get().read();
            if (!read)
                return rejected(read.error());
            auto cleared = read->withRead(
                [&]() -> FlowEditResult<void>
                {
                    clear(
                    ); // A terminal input cannot block later requests; payload destruction keeps the original gate.
                    return {};
                }
            );
            if (!cleared)
                return rejected(cleared.error());
            return cxx::unexpected(failure);
        }
        struct Properties final : lux::ui::Element
        {
            Impl& state_;
            std::string variable_name_{"value"}, linker_;
            std::size_t variable_type_{};
            Properties(object::ObjectDispatcherRef dispatcher, Impl& state)
                : Element(std::move(dispatcher), lux::ui::ElementId{"properties"}), state_(state),
                  linker_(state.state_.linker.executable.string())
            {
                setStretch({1, 1});
            }
            void insert(std::unique_ptr<lux::flowforge::Node> node)
            {
                const auto owner = state_.services_.metadata.view().code_lifetime;
                state_.status_ = state_.enqueue(
                    state_.display_.content,
                    [&]
                    {
                        return FlowInsertNode{
                            owner ? lux::object::CodeLease::plugin(owner) : lux::object::CodeLease::builtin(),
                            std::move(node)
                        };
                    }
                );
            }
            void draw() noexcept override
            {
                using namespace lux::flowforge;
                ImGui::BeginDisabled(!state_.binding_);
                if (ImGui::Button("Undo"))
                    state_.control_ = EControl::UNDO;
                ImGui::SameLine();
                if (ImGui::Button("Redo"))
                    state_.control_ = EControl::REDO;
                ImGui::BeginDisabled(!state_.binding_);
                if (ImGui::Button("Compile"))
                    state_.control_ = EControl::COMPILE;
                ImGui::EndDisabled();
                ImGui::InputText("Linker executable", &linker_);
                if (ImGui::Button("Retry link (fixed artifact)"))
                {
                    state_.state_.linker.executable = linker_;
                    ++state_.state_.linker.version;
                    state_.control_ = EControl::RETRY;
                }
                ImGui::BeginDisabled(!state_.binding_);
                if (ImGui::Button("Publish artifact"))
                    state_.control_ = EControl::PUBLISH;
                ImGui::EndDisabled();
                if (ImGui::Button("Cancel pending edits"))
                    state_.control_ = EControl::CANCEL;
                ImGui::TextWrapped("%s", state_.compile_status_.c_str());
                if (!state_.status_)
                    ImGui::TextUnformatted("Input rejected; Revert properties or Cancel pending edits to recover.");
                if (ImGui::InputText("Name", &state_.display_.name, ImGuiInputTextFlags_EnterReturnsTrue))
                    state_.status_ =
                        state_.enqueue(state_.display_.content, [&] { return FlowRename{state_.display_.name}; });
                if (ImGui::BeginCombo("Add node", "Choose node kind"))
                {
                    constexpr ENodeOperation choices[]{
                        ENodeOperation::ON_EVENT,
                        ENodeOperation::BRANCH,
                        ENodeOperation::SEQUENCE,
                        ENodeOperation::FOR_LOOP,
                        ENodeOperation::WHILE_LOOP,
                        ENodeOperation::BREAK,
                        ENodeOperation::RETURN,
                        ENodeOperation::FUNC_DEF_START,
                        ENodeOperation::ADD,
                        ENodeOperation::SUBTRACT,
                        ENodeOperation::MULTIPLY,
                        ENodeOperation::DIVIDE,
                        ENodeOperation::CMP_EQ,
                        ENodeOperation::CMP_NE,
                        ENodeOperation::CMP_LT,
                        ENodeOperation::CMP_LE,
                        ENodeOperation::CMP_GT,
                        ENodeOperation::CMP_GE
                    };
                    for (auto kind : choices)
                        if (ImGui::Selectable(toString(kind)))
                            insert(makeFlowNode(kind));
                    if (auto node = chooseRegisteredFlowNode(state_.services_.metadata.view()))
                        insert(std::move(node));
                    ImGui::EndCombo();
                }
                if (state_.properties_ && ImGui::CollapsingHeader("Selected node", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    auto& node = state_.properties_->value;
                    if (auto* signature = std::get_if<FlowSourceSignature>(&node.parameters))
                    {
                        ImGui::InputText("Function name", &node.name);
                        static_cast<void>(editFlowArguments("Arguments", signature->arguments));
                        static_cast<void>(editFlowArguments("Results", signature->results));
                        if (ImGui::Button("Apply signature"))
                            state_.status_ = state_.enqueue(
                                state_.properties_->based_on,
                                [&] { return FlowSetSignature{node.id, node.name, *signature}; }
                            );
                        if (ImGui::Button("Add call"))
                            state_.status_ = state_.enqueue(
                                state_.properties_->based_on,
                                [&] { return FlowInsertFunctionUse{node.id, false}; }
                            );
                        ImGui::SameLine();
                        if (ImGui::Button("Add return"))
                            state_.status_ = state_.enqueue(
                                state_.properties_->based_on,
                                [&] { return FlowInsertFunctionUse{node.id, true}; }
                            );
                    }
                    for (auto& pin : node.inputs)
                    {
                        ImGui::PushID(static_cast<int>(pin.id.value));
                        auto scalar = flowScalar(pin.literal);
                        ImGui::TextUnformatted(pin.name.c_str());
                        if (editFlowScalar(scalar))
                            pin.literal = flowScalarLiteral(scalar);
                        if (ImGui::SmallButton("Apply literal"))
                            state_.status_ = state_.enqueue(
                                state_.properties_->based_on,
                                [&] { return FlowSetLiteral{pin.id, pin.literal}; }
                            );
                        ImGui::PopID();
                    }
                    if (ImGui::Button("Revert properties"))
                        state_.control_ = EControl::REVERT;
                }
                if (ImGui::CollapsingHeader("Variables"))
                {
                    const auto types = flowScalarTypes();
                    ImGui::InputText("New name", &variable_name_);
                    if (ImGui::BeginCombo("Type", types[variable_type_]->name.data()))
                    {
                        for (std::size_t i{}; i < types.size(); ++i)
                            if (ImGui::Selectable(types[i]->name.data(), i == variable_type_))
                                variable_type_ = i;
                        ImGui::EndCombo();
                    }
                    if (ImGui::SmallButton("Add variable"))
                        state_.status_ = state_.enqueue(
                            state_.display_.content,
                            [&] {
                                return FlowAddVariable{
                                    variable_name_,
                                    std::string(types[variable_type_]->name),
                                    {EFlowLiteralKind::ZERO, {}}
                                };
                            }
                        );
                    for (auto& variable : state_.display_.variables)
                    {
                        ImGui::PushID(static_cast<int>(variable.id));
                        ImGui::InputText("Name", &variable.name);
                        auto scalar = flowScalar(variable.value);
                        if (editFlowScalar(scalar))
                            variable.value = flowScalarLiteral(scalar);
                        if (ImGui::SmallButton("Apply variable"))
                            state_.status_ =
                                state_.enqueue(state_.display_.content, [&] { return FlowSetVariable{variable}; });
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove variable"))
                            state_.status_ = state_.enqueue(
                                state_.display_.content,
                                [&] { return FlowRemoveVariable{variable.id}; }
                            );
                        ImGui::PopID();
                    }
                }
                if (ImGui::CollapsingHeader("Exports"))
                {
                    auto& entries = state_.display_.exports;
                    for (std::size_t i{}; i < entries.size(); ++i)
                    {
                        auto& entry = entries[i];
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::InputScalar("Identity", ImGuiDataType_U64, &entry.id.value);
                        ImGui::InputScalar("Symbol", ImGuiDataType_U64, &entry.symbol);
                        ImGui::InputScalar("Event node", ImGuiDataType_U64, &entry.entry_node_id.value);
                        for (std::size_t j{}; j < entry.binding_hints.size(); ++j)
                        {
                            auto& hint = entry.binding_hints[j];
                            ImGui::PushID(static_cast<int>(j));
                            int kind = static_cast<int>(hint.kind);
                            if (ImGui::Combo("Hint", &kind, "Hook\0Event\0"))
                                hint.kind = static_cast<script::EScriptBindingHintKind>(kind);
                            ImGui::InputText("Qualified name", &hint.qualified_name);
                            ImGui::PopID();
                        }
                        if (ImGui::SmallButton("Add hint"))
                            entry.binding_hints.emplace_back();
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove hint") && !entry.binding_hints.empty())
                            entry.binding_hints.pop_back();
                        ImGui::PopID();
                    }
                    if (ImGui::SmallButton("Add export"))
                    {
                        std::uint64_t next{1};
                        for (const auto& entry : entries)
                            if (entry.id.value >= next && entry.id.value != UINT64_MAX)
                                next = entry.id.value + 1;
                        entries.push_back({FlowForgeExportNodeId{next}, {}, next});
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove export") && !entries.empty())
                        entries.pop_back();
                    if (ImGui::Button("Apply exports"))
                        state_.status_ =
                            state_.enqueue(state_.display_.content, [&] { return FlowSetExports{entries}; });
                }
                ImGui::EndDisabled();
            }
        } properties_ui_;
        std::array<object::Connection, 2> connections_;
        Impl(FlowView& view, FlowViewServices services, FlowViewState state)
            : view_(view), services_(services), state_(std::move(state)),
              layout_(view.dispatcherRef(), lux::ui::ElementId{"content"}, lux::ui::ELayoutType::HORIZONTAL),
              graph_(view.dispatcherRef(), lux::ui::ElementId{"graph"}), properties_ui_(view.dispatcherRef(), *this)
        {
            graph_.setStretch({2, 1});
            for (auto* child : std::array<lux::ui::Element*, 2>{&graph_, &properties_ui_})
                if (auto result = layout_.addSubElement(*child); !result)
                {
                    status_ = rejected(result.error());
                    return;
                }
            if (auto result = view.setContent(layout_); !result)
            {
                status_ = rejected(result.error());
                return;
            }
            auto edit = object::LuxObject::connect(
                &graph_,
                &widgets::GraphCanvas::edited,
                [this](const widgets::CanvasEdit& value) noexcept
                {
                    if (canvas_edit_.size() >= 64)
                    {
                        status_ = rejected(views::EViewError::CAPACITY);
                        return;
                    }
                    const auto stage = value.cancelled ? ECanvasStage::CANCEL
                                       : value.began   ? ECanvasStage::BEGIN
                                                       : ECanvasStage::PREVIEW;
                    canvas_edit_.push_back({display_.content, value, {}, stage});
                    graph_.setEnabled(canvas_edit_.size() < 60);
                }
            );
            auto select = object::LuxObject::connect(
                &graph_,
                &widgets::GraphCanvas::selected,
                [this](std::span<const std::uint64_t> ids) noexcept { selected_ = ids.size() == 1 ? ids.front() : 0; }
            );
            if (!edit || !select)
                status_ = rejected(views::EViewError::CAPACITY);
            else
            {
                connections_[0] = std::move(*edit);
                connections_[1] = std::move(*select);
            }
        }
        ~Impl() noexcept
        {
            if (!services_.compilation.releaseResult(compile_))
                std::terminate(); // The view and its service share the owner thread.
            if (!discardInputs())
                std::terminate();
        }
        FlowViewResult<void> install(Display value)
        {
            if (!graph_.setGraph(std::move(value.nodes), std::move(value.links), canvas_edit_.empty()))
                return rejected(views::EViewError::BUSY);
            display_ = std::move(value);
            return {};
        }
        FlowViewResult<void> discardInputs()
        {
            const auto clear = [&]
            {
                properties_.reset();
                selected_.reset();
                canvas_edit_.clear();
                control_ = EControl::NONE;
                graph_.setEnabled(true);
            };
            if (!binding_)
            {
                clear();
                return {};
            }
            auto owner = services_.sessions.read(binding_->session);
            if (!owner)
            {
                if (owner.error() != sessions::ESessionError::STALE_SESSION)
                    return rejected(FlowEditError{owner.error()});
                clear();
                return {};
            }
            auto read = owner->get().read();
            if (!read)
                return rejected(read.error());
            return accepted(read->withRead(
                [&]() -> FlowEditResult<void>
                {
                    clear();
                    return {};
                }
            ));
        }
        FlowViewResult<void> rebind(std::optional<FlowViewBinding> binding)
        {
            if (!view_.isOnAffinityThread() || object::LuxObject::isDispatching())
                return rejected(views::EViewError::BUSY);
            if (binding == binding_)
                return {};
            Display candidate;
            if (binding)
            {
                if (!binding->interaction || binding->interaction->session() != binding->session)
                    return rejected(views::EViewError::INVALID_ID);
                auto session = services_.sessions.read(binding->session);
                if (!session)
                    return rejected(FlowEditError{session.error()});
                auto read = display(session->get());
                if (!read)
                    return cxx::unexpected(read.error());
                candidate = std::move(*read);
            }
            if (binding_)
            {
                auto ended = binding_->interaction->cancel();
                if (!ended)
                    return rejected(ended.error());
            }
            auto discarded = discardInputs();
            if (!discarded)
                return discarded;
            auto installed = install(std::move(candidate));
            if (!installed)
                return installed;
            if (!services_.compilation.releaseResult(compile_))
                std::terminate();
            compile_ = {};
            binding_ = binding;
            compile_status_.clear();
            control_ = EControl::NONE;
            return {};
        }
        FlowViewResult<void> select(std::uint64_t id)
        {
            if (!binding_)
                return rejected(views::EViewError::INVALID_ID);
            std::optional<NodePropertiesDraft> candidate;
            auto owner = services_.sessions.read(binding_->session);
            if (!owner)
                return rejected(FlowEditError{owner.error()});
            auto read = owner->get().read();
            if (!read)
                return rejected(read.error());
            const auto based_on = owner->get().describe().current;
            auto inspected = read->withRead(
                [&](const lux::flowforge::FlowSource& source) -> FlowEditResult<void>
                {
                    if (!id)
                        return {};
                    const auto found =
                        std::ranges::find(source.nodes, id, [](const auto& node) { return node.id.value; });
                    if (found == source.nodes.end())
                        return cxx::unexpected(FlowEditError{EFlowEditError::INVALID_SOURCE});
                    candidate.emplace(NodePropertiesDraft{based_on, *found});
                    return {};
                }
            );
            if (!inspected)
                return rejected(inspected.error());
            auto ended = binding_->interaction->cancel();
            if (!ended)
                return rejected(ended.error());
            auto selected = binding_->interaction->select(
                id ? std::vector{lux::flowforge::NodeId{id}} : std::vector<lux::flowforge::NodeId>{}
            );
            if (!selected)
                return rejected(selected.error());
            return accepted(read->withRead(
                [&]() -> FlowEditResult<void>
                {
                    canvas_edit_.clear();
                    properties_ = std::move(candidate);
                    graph_.setEnabled(true);
                    return {};
                }
            ));
        }
        FlowViewResult<void> maintain()
        {
            if (!binding_)
                return {};
            if (control_ == EControl::CANCEL)
            {
                auto cancelled = view_.cancelEdit();
                if (!cancelled)
                    return cancelled;
                status_ = {};
            }
            if (control_ == EControl::REVERT)
            {
                auto reverted = select(properties_ ? properties_->value.id.value : 0);
                if (reverted || !temporary(reverted.error()))
                    control_ = EControl::NONE;
                if (!reverted)
                    return reverted;
                status_ = {};
            }
            auto synchronized = binding_->interaction->synchronize();
            if (!synchronized)
                return rejected(synchronized.error());
            if (selected_)
            {
                auto selected = select(*selected_);
                if (!selected)
                    return selected;
                status_ = {};
                selected_.reset();
            }
            while (!canvas_edit_.empty())
            {
                auto& pending = canvas_edit_.front();
                const auto& request = pending.input;
                auto delivered = workbench::detail::deliverInput(
                    pending.stage,
                    request.committed,
                    [&] { return validate(pending.based_on); },
                    [&] { return accepted(binding_->interaction->cancel()); },
                    [&] { return view_.beginEdit("Move/connect graph nodes"); },
                    [&]() -> FlowViewResult<void>
                    {
                        if (pending.edits.empty())
                        {
                            std::visit(
                                [&](const auto& value)
                                {
                                    using T = std::decay_t<decltype(value)>;
                                    if constexpr (std::same_as<T, widgets::CanvasLink>)
                                        pending.edits.emplace_back(FlowConnect{{value.from}, {value.to}});
                                    else if constexpr (std::same_as<T, widgets::CanvasErase>)
                                    {
                                        FlowRemoveNodes removed;
                                        for (auto node : value.nodes)
                                            removed.nodes.push_back({node});
                                        for (auto link : value.links)
                                            removed.links.push_back({{link.from}, {link.to}});
                                        pending.edits.emplace_back(std::move(removed));
                                    }
                                    else
                                    {
                                        FlowMoveNodes moved;
                                        for (auto node : value.nodes)
                                            moved.value.push_back(
                                                {{node.node}, {node.position.x, node.position.y, true}}
                                            );
                                        pending.edits.emplace_back(std::move(moved));
                                    }
                                },
                                request.value
                            );
                        }
                        return view_.previewEdit(pending.edits);
                    },
                    [&]
                    {
                        auto committed = view_.commitEdit();
                        if (committed)
                            status_ = {};
                        return committed;
                    }
                );
                if (!delivered)
                    return pending.stage == ECanvasStage::CANCEL ? delivered : rejectInput(delivered.error());
                auto owner = services_.sessions.read(binding_->session);
                if (!owner)
                    return rejected(FlowEditError{owner.error()});
                auto read = owner->get().read();
                if (!read)
                    return rejected(read.error());
                auto cleared = read->withRead(
                    [&]() -> FlowEditResult<void>
                    {
                        canvas_edit_.pop_front(); // Replaced preview payloads die under the author gate.
                        return {};
                    }
                );
                if (!cleared)
                    return rejected(cleared.error());
            }
            graph_.setEnabled(true);
            FlowViewResult<void> controlled;
            switch (control_)
            {
            case EControl::UNDO:
                controlled = view_.undo();
                break;
            case EControl::REDO:
                controlled = view_.redo();
                break;
            case EControl::COMPILE:
                controlled = accepted(view_.compile());
                break;
            case EControl::RETRY:
                controlled = view_.retryLink(state_.linker);
                break;
            case EControl::PUBLISH:
                controlled = accepted(view_.requestPublication());
                break;
            case EControl::CANCEL:
                controlled = view_.cancelEdit();
                break;
            default:
                break;
            }
            if (controlled || !temporary(controlled.error()))
                control_ = EControl::NONE;
            if (!controlled)
                return controlled;
            auto owner = services_.sessions.read(binding_->session);
            if (!owner)
                return rejected(FlowEditError{owner.error()});
            const auto current = owner->get().describe().current;
            if (current != display_.content && !binding_->interaction->overlay())
            {
                auto read = display(owner->get());
                if (!read)
                    return cxx::unexpected(read.error());
                if (auto installed = install(std::move(*read)); !installed)
                    return installed;
            }
            if (compile_.value)
            {
                auto operation = services_.compilation.operation(compile_);
                if (!operation)
                    return rejected(operation.error());
                const auto& task = operation->get();
                compile_status_ = task.key().content != current ? "Compilation is stale"
                                  : task.ready()                ? "Compilation complete"
                                                                : "Compiling / linking";
                if (task.ready() && !task.result())
                    compile_status_ += "; failed (fixed artifact can be retried if available)";
            }
            return {};
        }
    };
    FlowView::FlowView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        FlowViewServices services,
        FlowViewState state
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{kViewDescriptor.type.name()}, "FlowForge"),
          impl_(std::make_unique<Impl>(*this, services, std::move(state)))
    {
    }
    FlowView::~FlowView() noexcept = default;
    FlowViewResult<std::unique_ptr<FlowView>> FlowView::create(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        FlowViewServices services,
        std::optional<FlowViewBinding> binding,
        FlowViewState state
    )
    {
        auto view = std::unique_ptr<FlowView>(new FlowView(dispatcher, std::move(id), services, std::move(state)));
        if (!view->status())
            return cxx::unexpected(view->status().error());
        auto bound = view->rebind(binding);
        if (!bound)
            return cxx::unexpected(bound.error());
        return view;
    }
    FlowViewResult<void> FlowView::rebind(std::optional<FlowViewBinding> binding)
    {
        return impl_->rebind(binding);
    }
    const std::optional<FlowViewBinding>& FlowView::binding() const noexcept
    {
        return impl_->binding_;
    }
    const FlowViewResult<void>& FlowView::status() const noexcept
    {
        return impl_->status_;
    }
    FlowCompileId FlowView::compilation() const noexcept
    {
        return impl_->compile_;
    }
    FlowViewResult<void> FlowView::rebindContent(const views::ViewContent& content)
    {
        const bool is_single = content.sessions.size() == 1 && content.primary == content.sessions.front();
        const bool is_invalid = !content.valid() || (!content.sessions.empty() && !is_single);
        if (is_invalid)
            return rejected(views::EViewError::INVALID_ID);
        if (impl_->binding_ && is_single && impl_->binding_->session.id() == *content.primary)
            return {};
        std::unique_ptr<FlowInteraction> interaction;
        std::optional<FlowViewBinding> binding;
        if (is_single)
        {
            auto key = impl_->services_.sessions.key(*content.primary);
            if (!key)
                return rejected(FlowEditError{key.error()});
            interaction = std::make_unique<FlowInteraction>(impl_->services_.sessions, *key);
            binding.emplace(*key, interaction.get());
        }
        auto adopted = rebind(binding);
        if (!adopted)
            return adopted;
        impl_->interaction_ = std::move(interaction);
        return {};
    }
    FlowViewResult<void> FlowView::beginEdit(std::string label)
    {
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        return accepted(impl_->binding_->interaction->begin(std::move(label)));
    }
    FlowViewResult<void> FlowView::previewEdit(std::vector<VFlowEdit>& edits)
    {
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        return accepted(impl_->binding_->interaction->preview(edits));
    }
    FlowViewResult<void> FlowView::commitEdit()
    {
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        return accepted(impl_->binding_->interaction->commit());
    }
    views::ViewCaptureResult FlowView::captureState() const
    {
        workspace::VersionedViewState result;
        serialization::BinaryWriter writer(result.bytes);
        const auto path = impl_->state_.linker.executable.u8string();
        if (path.size() > 32768)
            return cxx::unexpected(views::ViewPreparationFailure{"flow.view.state", 1, "Linker path too long", false});
        (void)writer.writeUnsigned(impl_->state_.linker.version);
        (void)writer.writeUnsigned(static_cast<std::uint32_t>(path.size()));
        (void)writer.writeBytes(std::as_bytes(std::span(path)));
        return result;
    }
    views::ViewStateResult FlowView::prepareState(std::uint32_t schema, std::span<const std::byte> bytes)
    {
        const auto invalid = [&] {
            return cxx::unexpected(
                views::ViewPreparationFailure{"flow.view.state", schema, "Invalid linker settings", false}
            );
        };
        if (schema != 1)
            return invalid();
        if (bytes.empty())
            return cxx::move_only_function<void()>{};
        serialization::BinaryReader reader(bytes);
        const auto version = reader.readUnsigned<std::uint64_t>();
        const auto size = reader.readUnsigned<std::uint32_t>();
        if (!version || !*version || !size || *size > 32768 || *size != reader.remaining())
            return invalid();
        std::string path(reinterpret_cast<const char*>(bytes.data() + reader.offset()), *size);
        if (path.find('\0') != std::string::npos)
            return invalid();
        // External layout bytes cross the platform path codec here. Invalid native conversion
        // is a preparation failure; the accepted view and its linker remain unchanged.
        try
        {
            FlowViewState candidate{{std::filesystem::u8path(path), *version}};
            return cxx::move_only_function<void()>{[this, candidate = std::move(candidate), path = std::move(path)](
                                                   ) mutable noexcept
                                                   {
                                                       impl_->state_ = std::move(candidate);
                                                       impl_->properties_ui_.linker_ = std::move(path);
                                                   }};
        }
        catch (const std::filesystem::filesystem_error&)
        {
            return invalid();
        }
    }
    FlowViewResult<void> FlowView::cancelEdit()
    {
        if (impl_->binding_)
        {
            auto ended = impl_->binding_->interaction->cancel();
            if (!ended)
                return rejected(ended.error());
        }
        return impl_->discardInputs();
    }
    FlowViewResult<void> FlowView::undo()
    {
        auto ended = cancelEdit();
        if (!ended)
            return ended;
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        auto owner = impl_->services_.sessions.edit(impl_->binding_->session);
        if (!owner)
            return rejected(FlowEditError{owner.error()});
        return accepted(owner->get().undo());
    }
    FlowViewResult<void> FlowView::redo()
    {
        auto ended = cancelEdit();
        if (!ended)
            return ended;
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        auto owner = impl_->services_.sessions.edit(impl_->binding_->session);
        if (!owner)
            return rejected(FlowEditError{owner.error()});
        return accepted(owner->get().redo());
    }
    FlowViewResult<FlowCompileId> FlowView::compile()
    {
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        auto owner = impl_->services_.sessions.read(impl_->binding_->session);
        if (!owner)
            return rejected(FlowEditError{owner.error()});
        auto snapshot = owner->get().capture();
        if (!snapshot)
            return rejected(snapshot.error());
        auto requested = impl_->services_.compilation.start(
            std::move(*snapshot),
            impl_->services_.metadata,
            {},
            impl_->state_.linker
        );
        if (!requested)
            return rejected(requested.error());
        if (!impl_->services_.compilation.releaseResult(impl_->compile_))
            std::terminate();
        impl_->compile_ = *requested;
        return *requested;
    }
    FlowViewResult<void> FlowView::retryLink(LinkSettings settings)
    {
        return accepted(impl_->services_.compilation.retryLink(impl_->compile_, std::move(settings)));
    }
    FlowViewResult<void> FlowView::requestPublication()
    {
        auto operation = impl_->services_.compilation.operation(impl_->compile_);
        if (!operation)
            return rejected(operation.error());
        auto compiled = operation->get().result();
        if (!compiled)
            return rejected(compiled.error());
        auto artifact = captureFlowArtifact(std::move(*compiled));
        if (!artifact)
            return rejected(artifact.error());
        auto sent = emit(publishRequested, *artifact);
        if (!sent.complete())
            return rejected(views::EViewError::BUSY);
        return {};
    }
    void FlowView::update() noexcept
    {
        if (auto result = impl_->maintain(); !result)
            impl_->status_ = cxx::unexpected(result.error());
    }
} // namespace lux::editor::flowforge

namespace lux::editor::flowforge
{
    FlowViewResult<views::DetachedView> makeFlowView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        FlowViewServices services,
        std::optional<FlowViewBinding> binding,
        FlowViewState state
    )
    {
        auto created = FlowView::create(dispatcher, std::move(id), services, binding, std::move(state));
        if (!created)
            return cxx::unexpected(created.error());
        const auto cancel = +[](lux::ui::Pane& pane) -> views::ViewCloseResult
        {
            auto ended = static_cast<FlowView&>(pane).cancelEdit();
            if (!ended)
            {
                const auto* edit = std::get_if<FlowEditError>(&ended.error());
                const bool is_session = edit && edit->code == EFlowEditError::SESSION;
                const auto* view = std::get_if<views::EViewError>(&ended.error());
                const auto code = is_session ? static_cast<std::uint64_t>(edit->session)
                                  : edit     ? static_cast<std::uint64_t>(edit->code)
                                             : static_cast<std::uint64_t>(*view);
                return cxx::unexpected(views::ViewPreparationFailure{
                    is_session ? "session"
                    : edit     ? "flowforge.edit"
                               : "view",
                    code,
                    "Interaction could not be ended",
                    temporary(ended.error())
                });
            }
            return {};
        };
        return views::DetachedView{
            lux::object::CodeLease::builtin(),
            std::move(*created),
            cancel,
            cancel,
            +[](lux::ui::Pane& pane, std::uint32_t schema, std::span<const std::byte> bytes)
            { return static_cast<FlowView&>(pane).prepareState(schema, bytes); },
            +[](const lux::ui::Pane& pane) { return static_cast<const FlowView&>(pane).captureState(); },
            +[](const lux::ui::Pane& pane) noexcept -> views::ViewContent
            {
                const auto& binding = static_cast<const FlowView&>(pane).binding();
                return binding ? views::ViewContent{{binding->session.id()}, binding->session.id()}
                               : views::ViewContent{};
            },
            +[](lux::ui::Pane& pane, const views::ViewContent& content) -> views::ViewCloseResult
            {
                auto adopted = static_cast<FlowView&>(pane).rebindContent(content);
                if (adopted)
                    return {};
                return cxx::unexpected(
                    workbench::detail::viewPreparationFailure(adopted.error(), temporary(adopted.error()))
                );
            }
        };
    }
} // namespace lux::editor::flowforge

namespace lux::editor::flowforge
{
    std::shared_ptr<views::ViewFactoryEntry> makeFlowViewFactory(
        flowforge::FlowViewServices flow,
        cxx::move_only_function<void(const persistence::DerivedArtifact&)> receiver
    )
    {
        using ArtifactIntent = cxx::move_only_function<void(const persistence::DerivedArtifact&)>;
        auto intent = std::make_shared<ArtifactIntent>(std::move(receiver));
        return workbench::detail::bindViewFactory<kViewDescriptor, views::ContentViewInput>(
            [flow, intent](const views::ViewFactoryInput& input, const views::ContentViewInput& value)
                -> views::ViewFactoryResult<views::DetachedView>
            {
                auto view = flowforge::makeFlowView(input.dispatcher(), input.paneId(), flow);
                if (!view)
                    return cxx::unexpected(workbench::detail::viewFailure(view.error()));
                auto bound = view->rebindContent(value.content);
                if (!bound)
                    return cxx::unexpected(views::ViewFactoryFailure{
                        views::EViewFactoryError::CONSTRUCT,
                        bound.error().domain,
                        bound.error().code,
                        bound.error().message
                    });
                auto connected =
                    workbench::detail::connectIntent(*view, &flowforge::FlowView::publishRequested, intent);
                if (!connected)
                    return cxx::unexpected(std::move(connected.error()));
                return std::move(*view);
            }
        );
    }
} // namespace lux::editor::flowforge
