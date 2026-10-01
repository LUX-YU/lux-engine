#include <lux/engine/editor/flowforge/FlowForgeEditorImpl.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <algorithm>
#include <charconv>
#include <lux/engine/editor/flowforge/FlowNodeControls.hpp>
#include <imgui.h>
#include <imgui_node_editor.h>
#include <imgui_stdlib.h>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>
#include <lux/engine/editor/widgets/NodeCanvasIds.hpp>
#include <lux/engine/editor/widgets/NodeCanvas.hpp>
#include <lux/engine/editor/ui/PublicationControls.hpp>
#include <lux/engine/editor/ui/AssetActions.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/configuration/EditorReflection.hpp>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <map>
#include <unordered_map>

namespace lux::editor::flowforge
{
    using namespace lux::editor::ui;
    namespace
    {
        namespace canvas = ax::NodeEditor;
        using widgets::CanvasDelete;
        using widgets::CanvasScope;
        using widgets::createCanvas;

    } // namespace

    class FlowForgeEditor::Impl::GraphElement final : public lux::ui::Element
    {
        using VScalar = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double>;
        struct LiteralField final
        {
            VScalar value;
            editing::StateId base;
            bool active{};
        };

        struct VariableDraft final
        {
            lux::flowforge::FlowSourceVariable source;
            VScalar value;
            editing::StateId base;
            bool dirty{};
        };
        void drawRegisteredNodes()
        {
            if (auto node = chooseRegisteredFlowNode(editor_.metadata()))
                accept(editor_.insertNode(node));
        }

        static VScalar zero(const lux::meta::RefType& type)
        {
            auto value = lux::meta::RuntimeObject::defaultOf(&type);
            if (!value)
            {
                return std::monostate{};
            }
            auto literal = lux::flowforge::captureFlowLiteral(value);
            return literal ? flowScalar(*literal) : VScalar{std::monostate{}};
        }

        struct FunctionDraft final
        {
            lux::flowforge::FlowSourceNode source;
            editing::StateId base;
            bool dirty{};
        };
        struct ExportsDraft final
        {
            std::vector<lux::flowforge::ExportMethodNode> values;
            editing::StateId base;
            bool dirty{};
        };

        void drawFunctions()
        {
            using O = lux::flowforge::ENodeOperation;
            if (!ImGui::CollapsingHeader("Functions and events"))
            {
                return;
            }
            const auto current = editor_.historyView()->history.current;
            if (function_index_state_ != current)
            {
                function_entries_.clear();
                for (const auto& node : editor_.nodes())
                {
                    const auto operation = editor_.nodeOperation(node.id);
                    if (operation == O::FUNC_DEF_START || operation == O::ON_EVENT)
                    {
                        function_entries_.push_back(node.id);
                    }
                }
                std::erase_if(function_drafts_, [&](const auto& entry) {
                    return std::ranges::find(function_entries_, entry.first) == function_entries_.end();
                });
                function_index_state_ = current;
            }
            ImGui::BeginDisabled(!editor_.project().writable());
            for (const auto id : function_entries_)
            {
                ImGui::PushID(reinterpret_cast<void*>(static_cast<std::uintptr_t>(id.value)));
                if (ImGui::TreeNode(
                        "entry",
                        "%.*s",
                        static_cast<int>(editor_.nodeName(id).size()),
                        editor_.nodeName(id).data()
                    ))
                {
                    auto found = function_drafts_.find(id);
                    if (found == function_drafts_.end() || (!found->second.dirty && found->second.base != current))
                    {
                        auto captured = editor_.captureNode(id);
                        if (accept(captured))
                        {
                            found = function_drafts_.insert_or_assign(id, FunctionDraft{std::move(*captured), current})
                                        .first;
                        }
                    }
                    if (found != function_drafts_.end())
                    {
                        auto& draft = found->second;
                        draft.dirty |= ImGui::InputText("Name", &draft.source.name);
                        auto& signature = std::get<lux::flowforge::FlowSourceSignature>(draft.source.parameters);
                        draft.dirty |= editFlowArguments("Arguments", signature.arguments);
                        const bool function = draft.source.operation == O::FUNC_DEF_START;
                        if (function)
                        {
                            draft.dirty |= editFlowArguments("Results", signature.results);
                        }
                        if (draft.dirty && draft.base != current)
                        {
                            ImGui::TextDisabled("Document changed. Revert this draft before applying.");
                        }
                        ImGui::BeginDisabled(!draft.dirty);
                        if (ImGui::Button("Apply signature"))
                        {
                            if (accept(editor_.setFunctionSignature(draft.base, id, draft.source.name, signature)))
                            {
                                draft.dirty = false;
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Revert"))
                        {
                            draft.dirty = false;
                            draft.base = {};
                        }
                        ImGui::EndDisabled();
                        if (function)
                        {
                            if (ImGui::SmallButton("Create call"))
                            {
                                accept(editor_.insertFunctionUse(id, false));
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Create return"))
                            {
                                accept(editor_.insertFunctionUse(id, true));
                            }
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::EndDisabled();
        }
        void drawExports()
        {
            if (!ImGui::CollapsingHeader("Exported entry points"))
            {
                return;
            }
            const auto current = editor_.historyView()->history.current;
            if (!exports_draft_.dirty && exports_draft_.base != current)
            {
                exports_draft_.values.assign(editor_.exports().begin(), editor_.exports().end());
                exports_draft_.base = current;
            }
            auto& draft = exports_draft_;
            ImGui::BeginDisabled(!editor_.project().writable());
            for (std::size_t index{}; index < draft.values.size(); ++index)
            {
                auto& value = draft.values[index];
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::BeginCombo("Event", editor_.nodeName(value.entry_node_id).data()))
                {
                    for (const auto& node : editor_.nodes())
                    {
                        if (editor_.nodeOperation(node.id) == lux::flowforge::ENodeOperation::ON_EVENT &&
                            ImGui::Selectable(editor_.nodeName(node.id).data(), node.id == value.entry_node_id))
                        {
                            value.entry_node_id = node.id;
                            draft.dirty = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                draft.dirty |= ImGui::InputScalar("Export identity", ImGuiDataType_U64, &value.id.value);
                draft.dirty |= ImGui::InputScalar(
                    "Symbol",
                    ImGuiDataType_U64,
                    &value.symbol,
                    nullptr,
                    nullptr,
                    "%016llX",
                    ImGuiInputTextFlags_CharsHexadecimal
                );
                for (std::size_t hint_index{}; hint_index < value.binding_hints.size(); ++hint_index)
                {
                    auto& hint = value.binding_hints[hint_index];
                    ImGui::PushID(static_cast<int>(hint_index));
                    int kind = static_cast<int>(hint.kind);
                    if (ImGui::Combo("Hint kind", &kind, "Hook\0Event\0"))
                    {
                        hint.kind = static_cast<lux::script::EScriptBindingHintKind>(kind);
                        draft.dirty = true;
                    }
                    draft.dirty |= ImGui::InputText("Qualified name", &hint.qualified_name);
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("Add binding hint"))
                {
                    value.binding_hints.emplace_back();
                    draft.dirty = true;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(value.binding_hints.empty());
                if (ImGui::SmallButton("Remove last hint"))
                {
                    value.binding_hints.pop_back();
                    draft.dirty = true;
                }
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::PopID();
            }
            if (ImGui::Button("Add export"))
            {
                std::uint64_t next{1};
                for (const auto& value : draft.values)
                {
                    if (value.id.value >= next && value.id.value != UINT64_MAX)
                    {
                        next = value.id.value + 1;
                    }
                }
                draft.values.push_back({lux::flowforge::FlowForgeExportNodeId{next}, {}, next});
                draft.dirty = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(draft.values.empty());
            if (ImGui::Button("Remove last export"))
            {
                draft.values.pop_back();
                draft.dirty = true;
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!draft.dirty);
            if (ImGui::Button("Apply exports") && unchanged(draft.base))
            {
                if (accept(editor_.setExports(draft.values)))
                {
                    draft.dirty = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert exports"))
            {
                draft.dirty = false;
                draft.base = {};
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }
        void drawVariables()
        {
            if (source_changed_)
            {
                std::erase_if(variable_drafts_, [this](const auto& entry) {
                    return std::ranges::find(
                               editor_.variables(),
                               entry.first,
                               &lux::flowforge::FlowGraph::GraphVariable::id
                           ) == editor_.variables().end();
                });
            }

            if (!ImGui::CollapsingHeader("Variables"))
            {
                return;
            }
            ImGui::BeginDisabled(!editor_.project().writable());
            ImGui::SetNextItemWidth(180);
            ImGui::InputText("New variable", &variable_name_);
            ImGui::SameLine();
            const auto types = flowScalarTypes();
            if (ImGui::BeginCombo("##new-variable-type", types[variable_type_]->name.data()))
            {
                for (std::size_t index{}; index < types.size(); ++index)
                {
                    if (ImGui::Selectable(types[index]->name.data(), index == variable_type_))
                    {
                        variable_type_ = index;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Add variable"))
            {
                if (accept(editor_.addVariable(
                        variable_name_,
                        types[variable_type_]->name,
                        flowScalarLiteral(zero(*types[variable_type_]))
                    )))
                {
                    variable_name_.clear();
                }
            }
            bool mutated{};
            for (const auto& variable : editor_.variables())
            {
                auto [position, inserted] = variable_drafts_.try_emplace(variable.id);
                auto& draft = position->second;
                if (inserted || (source_changed_ && !draft.dirty))
                {
                    auto captured = lux::flowforge::captureFlowVariable(variable);
                    if (!accept(captured))
                    {
                        continue;
                    }
                    draft.source = std::move(*captured);
                    draft.value = flowScalar(draft.source.value);
                }
                ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(variable.id)));
                bool changed{};
                ImGui::SetNextItemWidth(160);
                changed |= ImGui::InputText("##name", &draft.source.name);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110);
                if (ImGui::BeginCombo("##type", draft.source.type.c_str()))
                {
                    for (const auto* type : types)
                    {
                        if (ImGui::Selectable(type->name.data(), draft.source.type == type->name))
                        {
                            draft.source.type = type->name;
                            draft.value = zero(*type);
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140);
                changed |= editFlowScalar(draft.value);
                if (changed && !draft.dirty)
                {
                    draft.base = editor_.historyView()->history.current;
                    draft.dirty = true;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!draft.dirty);
                if (ImGui::SmallButton("Apply") && unchanged(draft.base))
                {
                    if (draft.value.index() != 0)
                    {
                        draft.source.value = flowScalarLiteral(draft.value);
                    }
                    if (accept(editor_.setVariable(draft.source)))
                    {
                        draft.dirty = false;
                        mutated = true;
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Revert"))
                {
                    auto captured = lux::flowforge::captureFlowVariable(variable);
                    if (accept(captured))
                    {
                        draft.source = std::move(*captured);
                        draft.value = flowScalar(draft.source.value);
                        draft.dirty = false;
                    }

                    mutated = true;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton("Get"))
                {
                    std::unique_ptr<lux::flowforge::Node> node = std::make_unique<lux::flowforge::GetVariableNode>(
                        variable.id,
                        lux::flowforge::DataPinInfo{variable.name, variable.type}
                    );
                    accept(editor_.insertNode(node));
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Set"))
                {
                    std::unique_ptr<lux::flowforge::Node> node = std::make_unique<lux::flowforge::SetVariableNode>(
                        variable.id,
                        lux::flowforge::DataPinInfo{variable.name, variable.type}
                    );
                    accept(editor_.insertNode(node));
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove"))
                {
                    mutated = accept(editor_.removeVariable(variable.id));
                }
                ImGui::PopID();
                if (mutated)
                {
                    break;
                }
            }
            ImGui::EndDisabled();
        }
        void drawLiteral(lux::flowforge::PinId pin)
        {
            const auto found = literals_.find(pin);
            if (found == literals_.end() || found->second.value.index() == 0)
            {
                return;
            }
            auto& field = found->second;
            ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(pin.value)));
            ImGui::SetNextItemWidth(160);
            const bool changed = editFlowScalar(field.value);
            if (ImGui::IsItemActivated())
            {
                field.base = editor_.historyView()->history.current;
                field.active = true;
            }
            const bool commit = field.value.index() == 1 ? changed : ImGui::IsItemDeactivatedAfterEdit();
            if (commit && unchanged(field.base))
            {
                auto literal = flowScalarLiteral(field.value);
                accept(editor_.setPinLiteral(pin, literal));
            }
            if (ImGui::IsItemDeactivated())
            {
                field.active = false;
                // Restore rejected or cancelled drafts from author data on the next draw.
                source_changed_ = true;
            }
            ImGui::PopID();
        }
        struct Position final
        {
            lux::graph::GraphNodeLayout author, display;
        };

    public:
        GraphElement(lux::ui::Element& parent, Impl& editor_, EditorResult<void>& status)
            : lux::ui::Element(parent, lux::ui::ElementId{"graph"}), editor_(editor_), canvas_(createCanvas()),
              name_(editor_.source_.name), content_connection_(lux::editor::detail::takeConnection(
                                               lux::object::LuxObject::connect(
                                                   editor_.editor_,
                                                   &flowforge::FlowForgeEditor::contentChanged,
                                                   [this](editing::Revision) noexcept { source_changed_ = true; }
                                               ),
                                               status
                                           ))
        {}

        [[nodiscard]] EditorResult<void> finishEditing()
        {
            const auto history_ = editor_.historyView();
            if (!history_)
                return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "graph.history", 0, {}, history_.error()}
                );
            const auto initial = history_->history.current;
            const auto stale = lux::cxx::unexpected(EditorFailure{
                EEditorError::BUSY,
                "graph.interaction",
                0,
                "The editor changed while this interaction was active. Resolve or cancel the retained draft."
            });
            if (name_active_ && name_base_ != initial)
                return stale;
            for (const auto& [id, field] : literals_)
                if (field.active && field.base != initial)
                    return stale;
            for (const auto& [id, draft] : variable_drafts_)
                if (draft.dirty && draft.base != initial)
                    return stale;
            for (const auto& [id, draft] : function_drafts_)
                if (draft.dirty && draft.base != initial)
                    return stale;
            if (exports_draft_.dirty && exports_draft_.base != initial)
                return stale;
            std::vector<lux::graph::GraphLayoutEntry> moved;
            auto* previous = canvas::GetCurrentEditor();
            canvas::SetCurrentEditor(canvas_.get());
            for (const auto& [node, position] : positions_)
            {
                if (!(editor_.nodeOperation(node) != lux::flowforge::ENodeOperation::INVALID))
                    continue;
                const auto value = canvas::GetNodePosition(canvas_ids_.node(node.value));
                if (value.x != position.display.x || value.y != position.display.y)
                    moved.push_back({node, {value.x, value.y, true}});
            }
            canvas::SetCurrentEditor(previous);
            if (!moved.empty() && move_base_ != initial)
                return stale;
            auto before = initial;
            const auto adopt = [&](const auto& result) -> EditorResult<void> {
                if (!result)
                    return lux::cxx::unexpected(
                        EditorFailure{EEditorError::INVALID_STATE, "graph.finish-editing", 0, {}, result.error()}
                    );
                const auto next = editor_.historyView()->history.current;
                if (name_base_ == before)
                    name_base_ = next;
                if (move_base_ == before)
                    move_base_ = next;
                for (auto& [id, field] : literals_)
                    if (field.base == before)
                        field.base = next;
                for (auto& [id, draft] : variable_drafts_)
                    if (draft.base == before)
                        draft.base = next;
                for (auto& [id, draft] : function_drafts_)
                    if (draft.base == before)
                        draft.base = next;
                if (exports_draft_.base == before)
                    exports_draft_.base = next;
                before = next;
                return {};
            };
            if (name_active_)
            {
                if (auto result = adopt(editor_.rename(name_)); !result)
                    return result;
                name_active_ = false;
            }
            for (auto& [pin, field] : literals_)
            {
                if (!field.active)
                    continue;
                if (auto result = adopt(editor_.setPinLiteral(pin, flowScalarLiteral(field.value))); !result)
                    return result;
                field.active = false;
            }
            for (auto& [id, draft] : function_drafts_)
            {
                if (!draft.dirty)
                    continue;
                const auto& signature = std::get<lux::flowforge::FlowSourceSignature>(draft.source.parameters);
                if (auto result = adopt(editor_.setFunctionSignature(draft.base, id, draft.source.name, signature));
                    !result)
                    return result;
                draft.dirty = false;
            }
            for (auto& [id, draft] : variable_drafts_)
            {
                if (!draft.dirty)
                    continue;
                if (draft.value.index() != 0)
                    draft.source.value = flowScalarLiteral(draft.value);
                if (auto result = adopt(editor_.setVariable(draft.source)); !result)
                    return result;
                draft.dirty = false;
            }
            if (exports_draft_.dirty)
            {
                if (auto result = adopt(editor_.setExports(exports_draft_.values)); !result)
                    return result;
                exports_draft_.dirty = false;
            }
            if (!moved.empty())
            {
                if (auto result = adopt(editor_.moveNodes(moved)); !result)
                    return result;
                for (const auto& move : moved)
                    positions_.at(move.node) = {move.layout, move.layout};
            }
            root().releaseFocus(*this);
            return {};
        }

        std::filesystem::path linkerPath() const
        {
            return std::filesystem::u8path(linker_);
        }
        void update() noexcept override
        {
            if (observed_history_ != editor_.historyId())
            {
                observed_history_ = editor_.historyId();
                auto* previous = canvas::GetCurrentEditor();
                canvas::SetCurrentEditor(canvas_.get());
                canvas::ClearSelection();
                canvas::SetCurrentEditor(previous);
                canvas_ids_ = {};
                positions_.clear();
                links_.clear();
                next_link_ = 1;
                pin_groups_.clear();
                literals_.clear();
                variable_drafts_.clear();
                function_drafts_.clear();
                exports_draft_ = {};
                function_entries_.clear();
                function_index_state_ = {};
                name_active_ = false;
                source_changed_ = true;
                name_ = editor_.source_.name;
                error_.clear();
            }
            if (const auto history_ = editor_.historyView();
                history_ && history_->history.revision != observed_revision_)
            {
                observed_revision_ = history_->history.revision;
                source_changed_ = true;
            }
        }

    private:
        Impl& editor_;

        template <class Result> bool accept(const Result& result)
        {
            if (result)
            {
                error_.clear();
                return true;
            }
            if constexpr (std::same_as<typename Result::error_type, editing::EditFailure>)
            {
                error_ = result.error().message.data();
                if (error_.empty())
                {
                    error_ = "Edit rejected: " + std::to_string(static_cast<unsigned>(result.error().code));
                }
            }
            else if constexpr (std::same_as<typename Result::error_type, lux::flowforge::FlowSourceFailure>)
            {
                error_ = "Variable: " + result.error().field;
            }
            else
            {
                error_ =
                    result.error().domain + ":" + std::to_string(result.error().reason) + " " + result.error().message;
            }
            return false;
        }
        bool unchanged(editing::StateId state)
        {
            const auto history_ = editor_.historyView();
            if (!history_ || history_->history.current != state)
            {
                error_ = "The graph changed during this gesture. Start the edit again.";
                return false;
            }
            return true;
        }
        lux::ui::SizeHint sizeHintContent() noexcept override
        {
            return {{320, 240}, {640, 480}};
        }
        void draw() noexcept override
        {

            if (!canvas_)
            {
                ImGui::TextUnformatted("Node canvas could not be created");
                return;
            }
            CanvasScope scope(canvas_.get());
            const auto history_ = editor_.historyView();
            if (!history_)
            {
                accept(history_);
                return;
            }
            ImGui::BeginDisabled(!editor_.project().writable());
            if (source_changed_ && !name_active_)
            {
                name_ = editor_.source_.name;
            }
            ImGui::SetNextItemWidth(240);
            ImGui::InputText("Name", &name_);
            if (ImGui::IsItemActivated())
            {
                name_base_ = history_->history.current;
                name_active_ = true;
            }
            if (ImGui::IsItemDeactivated())
            {
                if (ImGui::IsItemDeactivatedAfterEdit() && unchanged(name_base_))
                {
                    accept(editor_.rename(name_));
                }
                name_active_ = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Add node"))
            {
                ImGui::OpenPopup("node-palette");
            }
            if (ImGui::BeginPopup("node-palette"))
            {
                using O = lux::flowforge::ENodeOperation;
                constexpr std::array kinds{
                    O::ON_EVENT,
                    O::BRANCH,
                    O::SEQUENCE,
                    O::FOR_LOOP,
                    O::WHILE_LOOP,
                    O::BREAK,
                    O::RETURN,
                    O::FUNC_DEF_START,
                    O::ADD,
                    O::SUBTRACT,
                    O::MULTIPLY,
                    O::DIVIDE,
                    O::CMP_EQ,
                    O::CMP_NE,
                    O::CMP_LT,
                    O::CMP_LE,
                    O::CMP_GT,
                    O::CMP_GE
                };
                for (const auto kind : kinds)
                {
                    if (ImGui::Selectable(lux::flowforge::toString(kind)))
                    {
                        auto node = makeFlowNode(kind);
                        accept(editor_.insertNode(node));
                    }
                }
                drawRegisteredNodes();
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();

            drawVariables();
            drawFunctions();
            drawExports();
            if (ImGui::TreeNode("Compiler settings"))
            {
                ImGui::InputText("Linker", &linker_);
                ImGui::TextDisabled("%s", "Leave empty to use the configured system linker");
                ImGui::TreePop();
            }
            if (source_changed_)
            {
                pin_groups_.clear();
                for (const auto& pin : editor_.pins())
                {
                    pin_groups_[pin.owner].push_back(pin);
                    if (pin.direction == lux::graph::EPinDirection::INPUT && !editor_.pinType(pin.id).empty())
                    {
                        auto& field = literals_[pin.id];
                        if (!field.active)
                        {
                            const auto value = editor_.pinLiteral(pin.id);
                            field.value = value ? flowScalar(*value) : VScalar{std::monostate{}};
                        }
                    }
                }
                std::erase_if(literals_, [&](const auto& item) { return editor_.pinType(item.first).empty(); });
            }
            if (!error_.empty())
            {
                {
                    const auto& message_value = error_;
                    const std::string_view message{message_value};
                    ImGui::TextUnformatted(
                        message.empty() ? "" : message.data(),
                        message.empty() ? "" : message.data() + message.size()
                    );
                }
            }
            source_changed_ = false;
            drawCanvas();
        }
        void drawCanvas()
        {
            const bool writable = editor_.project().writable();
            canvas::Begin("FlowForge graph");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                move_base_ = editor_.historyView()->history.current;
            }
            std::size_t ordinal{};
            for (const auto& record : editor_.nodes())
            {
                const auto layout = editor_.nodeLayout(record.id);
                auto [position, inserted] = positions_.try_emplace(record.id);
                if (inserted || position->second.author != layout)
                {
                    position->second.author = layout;
                    position->second.display =
                        layout.placed
                            ? layout
                            : lux::graph::GraphNodeLayout{float(ordinal % 4) * 240, float(ordinal / 4) * 280, true};
                    canvas::SetNodePosition(
                        canvas_ids_.node(record.id.value),
                        {position->second.display.x, position->second.display.y}
                    );
                }
                ++ordinal;
                canvas::BeginNode(canvas_ids_.node(record.id.value));
                const auto name = editor_.nodeName(record.id);
                if (name.empty())
                {
                    ImGui::TextUnformatted(lux::flowforge::toString(editor_.nodeOperation(record.id)));
                }
                else
                {
                    ImGui::TextUnformatted(name.data(), name.data() + name.size());
                }
                for (const auto& pin : pin_groups_[record.id])
                {
                    const bool input = pin.direction == lux::graph::EPinDirection::INPUT;
                    canvas::BeginPin(
                        canvas_ids_.pin(pin.id.value),
                        input ? canvas::PinKind::Input : canvas::PinKind::Output
                    );
                    const auto label = editor_.pinName(pin.id);
                    const auto type = editor_.pinType(pin.id);
                    ImGui::Text(
                        "%s %.*s %.*s",
                        input ? "<" : ">",
                        int(label.size()),
                        label.data(),
                        int(type.size()),
                        type.data()
                    );
                    if (input && !type.empty())
                    {
                        ImGui::BeginDisabled(!writable);
                        drawLiteral(pin.id);
                        ImGui::EndDisabled();
                    }
                    canvas::EndPin();
                }
                canvas::EndNode();
            }
            const auto links = editor_.links();
            for (const auto& link : links)
            {
                const auto key = std::pair{link.from.value, link.to.value};
                auto [found, inserted] = links_.try_emplace(key, next_link_);
                if (inserted)
                {
                    ++next_link_;
                }
                canvas::Link(
                    canvas_ids_.link(found->second),
                    canvas_ids_.pin(link.from.value),
                    canvas_ids_.pin(link.to.value)
                );
            }
            lux::graph::LinkRecord created;
            if (writable && canvas::BeginCreate())
            {
                canvas::PinId first, second;
                if (canvas::QueryNewLink(&first, &second) && first && second && canvas::AcceptNewItem())
                {
                    created = {{canvas_ids_.source(first)}, {canvas_ids_.source(second)}};
                    const auto pins = editor_.pins();
                    const auto pin = std::ranges::find(pins, created.from, &lux::graph::PinRecord::id);
                    if (pin != pins.end() && pin->direction == lux::graph::EPinDirection::INPUT)
                    {
                        std::swap(created.from, created.to);
                    }
                }
                canvas::EndCreate();
            }
            std::vector<lux::flowforge::NodeId> removed_nodes;
            std::vector<lux::graph::LinkRecord> removed_links;
            if (writable && canvas::BeginDelete())
            {
                canvas::NodeId node;
                while (canvas::QueryDeletedNode(&node))
                {
                    if (canvas::AcceptDeletedItem())
                    {
                        removed_nodes.push_back(lux::flowforge::NodeId{canvas_ids_.source(node)});
                    }
                }
                canvas::LinkId link;
                while (canvas::QueryDeletedLink(&link))
                {
                    const auto found = std::ranges::find_if(links_, [&](const auto& value) {
                        return value.second == canvas_ids_.source(link);
                    });
                    if (found != links_.end() && canvas::AcceptDeletedItem())
                    {
                        removed_links.push_back({{found->first.first}, {found->first.second}});
                    }
                }
                canvas::EndDelete();
            }
            canvas::End();
            if (!removed_nodes.empty() || !removed_links.empty())
            {
                accept(editor_.removeNodes(removed_nodes, removed_links));
            }
            if (created.from.valid() && created.to.valid())
            {
                accept(editor_.connect(created.from, created.to));
            }
            if (writable && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                std::vector<lux::graph::GraphLayoutEntry> moved;
                for (auto& [node, position] : positions_)
                {
                    if (editor_.nodeOperation(node) == lux::flowforge::ENodeOperation::INVALID)
                    {
                        continue;
                    }
                    const auto value = canvas::GetNodePosition(canvas_ids_.node(node.value));
                    if (value.x != position.display.x || value.y != position.display.y)
                    {
                        moved.push_back({node, {value.x, value.y, true}});
                    }
                }
                if (!moved.empty() && (!unchanged(move_base_) || !accept(editor_.moveNodes(moved))))
                {
                    for (const auto& item : moved)
                    {
                        const auto& position = positions_.at(item.node).display;
                        canvas::SetNodePosition(canvas_ids_.node(item.node.value), {position.x, position.y});
                    }
                }
            }
            std::erase_if(positions_, [&](const auto& item) {
                return editor_.nodeOperation(item.first) == lux::flowforge::ENodeOperation::INVALID;
            });
            std::erase_if(links_, [&](const auto& item) {
                return std::ranges::find(
                           editor_.links(),
                           lux::graph::LinkRecord{{item.first.first}, {item.first.second}}
                       ) == editor_.links().end();
            });
        }
        widgets::NodeCanvasIds canvas_ids_;
        std::unique_ptr<canvas::EditorContext, CanvasDelete> canvas_;
        std::unordered_map<lux::flowforge::NodeId, Position> positions_;
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> links_;
        std::uint64_t next_link_{1};
        std::unordered_map<lux::flowforge::NodeId, std::vector<lux::graph::PinRecord>> pin_groups_;
        std::unordered_map<lux::flowforge::PinId, LiteralField> literals_;
        std::unordered_map<std::uint64_t, VariableDraft> variable_drafts_;
        std::unordered_map<lux::flowforge::NodeId, FunctionDraft> function_drafts_;
        ExportsDraft exports_draft_;
        std::vector<lux::flowforge::NodeId> function_entries_;
        editing::StateId function_index_state_;
        std::string variable_name_;
        std::size_t variable_type_{6};
        std::string name_, error_, linker_;
        editing::StateId name_base_, move_base_;
        bool source_changed_{true}, name_active_{};
        editing::Revision observed_revision_;
        editing::HistoryId observed_history_;
        object::Connection content_connection_;
    };

    class FlowForgeEditor::Impl::ContentElement final : public lux::ui::Element
    {
    public:
        explicit ContentElement(Impl& editor_, EditorResult<void>& status)
            : lux::ui::Element(*editor_.editor_, lux::ui::ElementId{"content"}), editor_(editor_),
              layout_(*this, lux::ui::ElementId{"layout"}),
              asset_actions_(layout_, *editor_.editor_, editor_.project(), status),
              work_(layout_, lux::ui::ElementId{"work"}, lux::ui::ELayoutType::HORIZONTAL),
              graph_(work_, editor_, status), side_(work_, lux::ui::ElementId{"compilation"}),
              actions_(side_, lux::ui::ElementId{"actions"}, lux::ui::ELayoutType::HORIZONTAL),
              compile_button_(actions_, lux::ui::ElementId{"compile"}, "Compile"),
              publish_button_(actions_, lux::ui::ElementId{"publish"}, "Save source and publish asset"),
              retry_button_(actions_, lux::ui::ElementId{"retry-link"}, "Retry link"),
              status_(side_, lux::ui::ElementId{"status"}, "Not compiled"),
              publication_(side_, lux::ui::ElementId{"publication"}, *editor_.editor_, status),
              compile_connection_(lux::editor::detail::takeConnection(
                  lux::object::LuxObject::connect(
                      std::addressof(compile_button_),
                      &lux::ui::Button::activated,
                      [this]() noexcept { action_ = EAction::COMPILE; }
                  ),
                  status
              )),
              publish_connection_(lux::editor::detail::takeConnection(
                  lux::object::LuxObject::connect(
                      std::addressof(publish_button_),
                      &lux::ui::Button::activated,
                      [this]() noexcept { action_ = EAction::PUBLISH; }
                  ),
                  status
              )),
              retry_connection_(lux::editor::detail::takeConnection(
                  lux::object::LuxObject::connect(
                      std::addressof(retry_button_),
                      &lux::ui::Button::activated,
                      [this]() noexcept { action_ = EAction::RETRY; }
                  ),
                  status
              ))
        {
            graph_.setStretch({2, 1});
            actions_.setStretch({1, 0});
            publish_button_.setEnabled(false);
            retry_button_.setVisible(false);
        }
        EditorResult<void> finishEditing()
        {
            return graph_.finishEditing();
        }
        void command(object::EventView& event) noexcept
        {
            asset_actions_.command(event);
        }
        void applyControls() noexcept
        {
            if (action_ == EAction::NONE)
                return;
            const auto finished = editor_.finishEditing();
            if (!finished)
            {
                status_.setText(finished.error().message);
                return;
            }
            if (action_ == EAction::COMPILE)
            {
                const auto request = editor_.requestCompile(graph_.linkerPath());
                if (request)
                {
                    compile_ = *request;
                    error_.clear();
                }
                else
                    error_ = request.error().message;
            }
            else if (action_ == EAction::RETRY)
            {
                const auto request = editor_.retryLink(compile_, graph_.linkerPath());
                if (request)
                    error_.clear();
                else
                    error_ = request.error().message;
            }
            else
            {
                const auto request = editor_.requestPublish(compile_, "desktop");
                if (request)
                {
                    publication_.track(*request);
                    error_.clear();
                }
                else
                    error_ = request.error().message;
            }
            action_ = EAction::NONE;
        }

    private:
        lux::ui::SizeHint sizeHintContent() noexcept override
        {
            return layout_.sizeHint();
        }
        lux::ui::SizeHint measureContent(float width) noexcept override
        {
            return layout_.measure(width);
        }
        void arrangeContent() noexcept override
        {
            layout_.arrange({{}, rect().size});
        }
        void draw() noexcept override
        {
            drawChild(layout_);
        }
        void update() noexcept override
        {
            const bool available = !editor_.source_.id.isNull() && editor_.asset_status_.phase == EAssetEditPhase::IDLE;
            graph_.setEnabled(available);
            compile_button_.setEnabled(available);
            publish_button_.setEnabled(false);
            retry_button_.setVisible(false);
            if (!error_.empty())
            {
                status_.setText(error_);
                return;
            }
            if (!static_cast<bool>(compile_))
                return;
            const auto state = editor_.compileStatus(compile_);
            if (!state)
            {
                if (state.error().code == EEditorError::STALE_REQUEST)
                {
                    compile_ = {};
                    error_.clear();
                    status_.setText("Not compiled");
                }
                else
                    status_.setText(state.error().message);
                return;
            }
            compile_button_.setEnabled(!std::holds_alternative<flowforge::FlowCompilePending>(*state));
            if (const auto* success = std::get_if<flowforge::FlowCompileSucceeded>(&*state))
            {
                status_.setText(
                    success->current ? "Compilation succeeded" : "Changed; compile again to update the result"
                );
                publish_button_.setEnabled(
                    success->current && editor_.project().writable() && editor_.saveRequests().empty()
                );
            }
            else if (const auto* failure = std::get_if<flowforge::FlowCompileFailed>(&*state))
            {
                status_.setText(failure->failure.message);
                retry_button_.setVisible(failure->retryable);
            }
            else
                status_.setText("Compiling...");
        }
        Impl& editor_;
        enum class EAction : std::uint8_t
        {
            NONE,
            COMPILE,
            PUBLISH,
            RETRY
        };
        EAction action_{};
        lux::process::TaskId compile_;
        std::string error_;
        lux::ui::Layout layout_;
        TAssetActions<flowforge::FlowForgeEditor> asset_actions_;
        lux::ui::Layout work_;
        GraphElement graph_;
        lux::ui::Layout side_, actions_;
        lux::ui::Button compile_button_, publish_button_, retry_button_;
        lux::ui::Label status_;
        TPublicationControls<flowforge::FlowForgeEditor> publication_;
        object::Connection compile_connection_, publish_connection_, retry_connection_;
    };
} // namespace lux::editor::flowforge

namespace lux::editor::flowforge
{
    void FlowForgeEditor::Impl::createContent(EditorResult<void>& status)
    {
        content_ = std::make_unique<ContentElement>(*this, status);
        if (!status)
            return;
        editor_->setContent(*content_);
        editor_->root().setDockLayout(
            {{},
             std::string(editor_->id().name()),
             {},
             {},
             0,
             0,
             0,
             editor_->root().findPane(lux::ui::PaneIdView{"project"}) ? "project" : ""}
        );
    }
    EditorResult<void> FlowForgeEditor::Impl::finishContentEditing()
    {
        return content_ ? content_->finishEditing() : EditorResult<void>{};
    }
    void FlowForgeEditor::Impl::contentCommand(object::EventView& event) noexcept
    {
        if (content_)
            content_->command(event);
    }
    void FlowForgeEditor::Impl::applyContentIntents() noexcept
    {
        if (content_)
            content_->applyControls();
    }
}

namespace lux::editor::flowforge
{
    FlowForgeEditor::Impl::Impl(EditorContext& context)
        : editor_context_(context),
          completion_work_(
              context.execution(),
              this,
              [](void* owner) noexcept { static_cast<Impl*>(owner)->completion_pending_ = true; }
          ),
          compilations_(context.execution(), 2)
    {}
    FlowForgeEditor::Impl::~Impl()
    {
        reading_ = {};
        static_cast<void>(compilations_.cancel(compilation_));
        completion_work_.cancel();
    }

}
