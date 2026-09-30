#include <lux/engine/editor/flowforge/FlowView.hpp>
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
                [](const auto& error) {
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
            auto current = read->withRead([&](const lux::flowforge::FlowSource& source) -> FlowEditResult<Display> {
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
            });
            if (!current)
                return rejected(current.error());
            return std::move(*current);
        }
    }
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
        enum class ECanvasStage : std::uint8_t
        {
            BEGIN,
            PREVIEW,
            COMMIT,
            CANCEL,
            COMPLETE
        };
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
            const auto clear = [&] {
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
            auto cleared = read->withRead([&]() -> FlowEditResult<void> {
                clear(); // A terminal input cannot block later requests; payload destruction keeps the original gate.
                return {};
            });
            if (!cleared)
                return rejected(cleared.error());
            return cxx::unexpected(failure);
        }
        struct Properties final : lux::ui::Element
        {
            Impl& state_;
            std::string variable_name_{"value"}, linker_;
            std::size_t variable_type_{};
            Properties(lux::ui::Element& parent, Impl& state)
                : Element(parent, lux::ui::ElementId{"properties"}), state_(state),
                  linker_(state.state_.linker.executable.string())
            {
                setStretch({1, 1});
            }
            void insert(std::unique_ptr<lux::flowforge::Node> node)
            {
                const auto& owner = state_.services_.metadata.code_lifetime;
                state_.status_ = state_.enqueue(state_.display_.content, [&] {
                    return FlowInsertNode{
                        owner ? contracts::CodeLease::plugin(owner) : contracts::CodeLease::builtin(),
                        std::move(node)
                    };
                });
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
                ImGui::BeginDisabled(!state_.services_.compile);
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
                ImGui::BeginDisabled(!state_.services_.publish);
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
                    if (auto node = chooseRegisteredFlowNode(state_.services_.metadata))
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
                            state_.status_ = state_.enqueue(state_.properties_->based_on, [&] {
                                return FlowSetSignature{node.id, node.name, *signature};
                            });
                        if (ImGui::Button("Add call"))
                            state_.status_ = state_.enqueue(state_.properties_->based_on, [&] {
                                return FlowInsertFunctionUse{node.id, false};
                            });
                        ImGui::SameLine();
                        if (ImGui::Button("Add return"))
                            state_.status_ = state_.enqueue(state_.properties_->based_on, [&] {
                                return FlowInsertFunctionUse{node.id, true};
                            });
                    }
                    for (auto& pin : node.inputs)
                    {
                        ImGui::PushID(static_cast<int>(pin.id.value));
                        auto scalar = flowScalar(pin.literal);
                        ImGui::TextUnformatted(pin.name.c_str());
                        if (editFlowScalar(scalar))
                            pin.literal = flowScalarLiteral(scalar);
                        if (ImGui::SmallButton("Apply literal"))
                            state_.status_ = state_.enqueue(state_.properties_->based_on, [&] {
                                return FlowSetLiteral{pin.id, pin.literal};
                            });
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
                        state_.status_ = state_.enqueue(state_.display_.content, [&] {
                            return FlowAddVariable{
                                variable_name_,
                                std::string(types[variable_type_]->name),
                                {EFlowLiteralKind::ZERO, {}}
                            };
                        });
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
                            state_.status_ = state_.enqueue(state_.display_.content, [&] {
                                return FlowRemoveVariable{variable.id};
                            });
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
              layout_(view, lux::ui::ElementId{"content"}, lux::ui::ELayoutType::HORIZONTAL),
              graph_(layout_, lux::ui::ElementId{"graph"}), properties_ui_(layout_, *this)
        {
            graph_.setStretch({2, 1});
            view.setContent(layout_);
            auto edit = object::LuxObject::connect(
                &graph_,
                &widgets::GraphCanvas::edited,
                [this](const widgets::CanvasEdit& value) noexcept {
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
            if (!discardInputs())
                std::terminate();
        }
        void install(Display value)
        {
            graph_.setGraph(std::move(value.nodes), std::move(value.links));
            display_ = std::move(value);
        }
        FlowViewResult<void> discardInputs()
        {
            const auto clear = [&] {
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
            return accepted(read->withRead([&]() -> FlowEditResult<void> {
                clear();
                return {};
            }));
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
            binding_ = binding;
            compile_ = {};
            compile_status_.clear();
            control_ = EControl::NONE;
            install(std::move(candidate));
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
            auto inspected = read->withRead([&](const lux::flowforge::FlowSource& source) -> FlowEditResult<void> {
                if (!id)
                    return {};
                const auto found = std::ranges::find(source.nodes, id, [](const auto& node) { return node.id.value; });
                if (found == source.nodes.end())
                    return cxx::unexpected(FlowEditError{EFlowEditError::INVALID_SOURCE});
                candidate.emplace(NodePropertiesDraft{based_on, *found});
                return {};
            });
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
            return accepted(read->withRead([&]() -> FlowEditResult<void> {
                canvas_edit_.clear();
                properties_ = std::move(candidate);
                graph_.setEnabled(true);
                return {};
            }));
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
                const bool requires_source =
                    pending.stage != ECanvasStage::CANCEL && pending.stage != ECanvasStage::COMPLETE;
                if (requires_source)
                {
                    auto admitted = validate(pending.based_on);
                    if (!admitted)
                        return rejectInput(admitted.error());
                }
                if (pending.stage == ECanvasStage::CANCEL)
                {
                    auto ended = accepted(binding_->interaction->cancel());
                    if (!ended)
                        return ended;
                    pending.stage = ECanvasStage::COMPLETE;
                }
                if (pending.stage == ECanvasStage::BEGIN)
                {
                    auto begun = view_.beginEdit("Move/connect graph nodes");
                    if (!begun)
                        return rejectInput(begun.error());
                    pending.stage = ECanvasStage::PREVIEW;
                }
                if (pending.stage == ECanvasStage::PREVIEW)
                {
                    if (pending.edits.empty())
                    {
                        std::visit(
                            [&](const auto& value) {
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
                                        moved.value.push_back({{node.node}, {node.position.x, node.position.y, true}});
                                    pending.edits.emplace_back(std::move(moved));
                                }
                            },
                            request.value
                        );
                    }
                    auto previewed = view_.previewEdit(pending.edits);
                    if (!previewed)
                        return rejectInput(previewed.error());
                    pending.stage = request.committed ? ECanvasStage::COMMIT : ECanvasStage::COMPLETE;
                }
                if (pending.stage == ECanvasStage::COMMIT)
                {
                    auto committed = view_.commitEdit();
                    if (!committed)
                        return rejectInput(committed.error());
                    pending.stage = ECanvasStage::COMPLETE;
                    status_ = {};
                }
                auto owner = services_.sessions.read(binding_->session);
                if (!owner)
                    return rejected(FlowEditError{owner.error()});
                auto read = owner->get().read();
                if (!read)
                    return rejected(read.error());
                auto cleared = read->withRead([&]() -> FlowEditResult<void> {
                    canvas_edit_.pop_front(); // Replaced preview payloads die under the author gate.
                    return {};
                });
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
                controlled = view_.publish();
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
                install(std::move(*read));
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
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.flowforge"}, "FlowForge"),
          impl_(std::make_unique<Impl>(*this, services, std::move(state)))
    {}
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
        if (!impl_->binding_ || !impl_->services_.compile)
            return rejected(views::EViewError::INVALID_ID);
        auto requested = impl_->services_.compile(impl_->services_.request_owner, impl_->binding_->session);
        if (!requested)
            return rejected(requested.error());
        impl_->compile_ = *requested;
        return *requested;
    }
    FlowViewResult<void> FlowView::retryLink(LinkSettings settings)
    {
        return accepted(impl_->services_.compilation.retryLink(impl_->compile_, std::move(settings)));
    }
    FlowViewResult<void> FlowView::publish()
    {
        if (!impl_->binding_ || !impl_->services_.publish)
            return rejected(views::EViewError::INVALID_ID);
        return accepted(impl_->services_.publish(impl_->services_.request_owner, impl_->compile_));
    }
    void FlowView::update() noexcept
    {
        if (auto result = impl_->maintain(); !result)
            impl_->status_ = cxx::unexpected(result.error());
    }
}
