#include <algorithm>
#include <charconv>
#include <imgui.h>
#include <imgui_node_editor.h>
#include <imgui_stdlib.h>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>
#include <lux/engine/editor/gui/DocumentPane.hpp>
#include <lux/engine/editor/gui/GuiDocumentProvider.hpp>
#include <lux/engine/editor/gui/NodeCanvasIds.hpp>
#include <lux/engine/editor/gui/PublicationControls.hpp>
#include <lux/engine/editor/gui/flowforge/FlowForgeDocumentProvider.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <map>
#include <unordered_map>

namespace lux::editor::gui
{
namespace
{
namespace canvas = ax::NodeEditor;
struct CanvasDelete final
{
    void operator()(canvas::EditorContext *context) const noexcept
    {
        canvas::DestroyEditor(context);
    }
};
struct CanvasScope final
{
    canvas::EditorContext *previous{canvas::GetCurrentEditor()};
    explicit CanvasScope(canvas::EditorContext *context)
    {
        canvas::SetCurrentEditor(context);
    }
    ~CanvasScope()
    {
        canvas::SetCurrentEditor(previous);
    }
};
std::unique_ptr<canvas::EditorContext, CanvasDelete> createCanvas()
{
    canvas::Config config;
    config.SettingsFile = nullptr;
    // Resizing the surrounding Pane preserves the user's graph zoom.
    config.CanvasSizeMode = canvas::CanvasSizeMode::CenterOnly;
    return std::unique_ptr<canvas::EditorContext, CanvasDelete>(canvas::CreateEditor(&config));
}
std::unique_ptr<lux::flowforge::Node> createNode(lux::flowforge::ENodeOperation operation)
{
    using namespace lux::flowforge;
    switch (operation)
    {
    case ENodeOperation::ON_EVENT:
        return std::make_unique<OnEventNode>(0, "event");
    case ENodeOperation::BRANCH:
        return std::make_unique<BranchNode>(0);
    case ENodeOperation::SEQUENCE:
        return std::make_unique<SequenceNode>(0);
    case ENodeOperation::FOR_LOOP:
        return std::make_unique<ForLoopNode>(0);
    case ENodeOperation::WHILE_LOOP:
        return std::make_unique<WhileLoopNode>(0);
    case ENodeOperation::BREAK:
        return std::make_unique<BreakNode>(0);
    case ENodeOperation::RETURN:
        return std::make_unique<ReturnNode>(0);
    case ENodeOperation::FUNC_DEF_START:
        return std::make_unique<FuncDefNode>(0, "function", std::vector<FuncArgInfo>{});
    default:
        return std::make_unique<BinaryOpNode>(0, operation, lux::meta::builtin_ref_type_ptr<double>());
    }
}

class FlowForgePane final : public DocumentPane<FlowForgePane, flowforge::FlowForgeEditor>
{
    using Scalar = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double>;
    struct LiteralField final
    {
        Scalar value;
        editing::StateId base;
        bool active{};
    };
    static Scalar scalar(const lux::flowforge::FlowSourceLiteral &literal)
    {
        using K = lux::flowforge::EFlowLiteralKind;
        if (literal.kind == K::BOOLEAN)
        {
            return literal.value == "true";
        }
        const auto parse = [&]<class T>() -> Scalar {
            T value{};
            const auto begin = literal.value.data(), end = begin + literal.value.size();
            const auto result = std::from_chars(begin, end, value);
            if (result.ec == std::errc{} && result.ptr == end)
            {
                return value;
            }
            return std::monostate{};
        };
        switch (literal.kind)
        {
        case K::SIGNED:
            return parse.template operator()<std::int64_t>();
        case K::UNSIGNED:
            return parse.template operator()<std::uint64_t>();
        case K::REAL:
            return parse.template operator()<double>();
        default:
            return std::monostate{};
        }
    }
    struct VariableDraft final
    {
        lux::flowforge::FlowSourceVariable source;
        Scalar value;
        editing::StateId base;
        bool dirty{};
    };
    void drawRegisteredNodes()
    {
        using namespace lux::flowforge;
        const auto &metadata = document_.metadata();
        if (ImGui::BeginMenu("Native functions", !metadata.functions.empty()))
        {
            for (const auto *function : metadata.functions)
            {
                ImGui::PushID(function);
                const auto &info = function->invokable;
                ImGui::TextUnformatted(info.full_name.data(), info.full_name.data() + info.full_name.size());
                ImGui::SameLine();
                if (ImGui::Selectable(std::string(info.type_signature).c_str()))
                {
                    std::unique_ptr<Node> node = std::make_unique<NativeFuncCall>(0, *function);
                    accept(document_.insertNode(node));
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Classes", !metadata.classes.empty()))
        {
            for (const auto *type : metadata.classes)
            {
                ImGui::PushID(type);
                if (ImGui::BeginMenu(std::string(type->full_name).c_str()))
                {
                    for (const auto &field : type->fields)
                    {
                        if (field.visibility != lux::meta::EVisibility::Public || field.is_volatile)
                        {
                            continue;
                        }
                        ImGui::PushID(&field);
                        ImGui::TextUnformatted(field.name.data(), field.name.data() + field.name.size());
                        ImGui::SameLine();
                        if (ImGui::Selectable("Read"))
                        {
                            std::unique_ptr<Node> node = std::make_unique<GetFieldNode>(0, *type, field);
                            accept(document_.insertNode(node));
                        }
                        if (!field.is_const && ImGui::Selectable("Write"))
                        {
                            std::unique_ptr<Node> node = std::make_unique<SetFieldNode>(0, *type, field);
                            accept(document_.insertNode(node));
                        }
                        ImGui::PopID();
                    }
                    for (const auto &method : type->methods)
                    {
                        if (method.visibility != lux::meta::EVisibility::Public)
                        {
                            continue;
                        }
                        ImGui::PushID(&method);
                        const auto &info = method.invokable;
                        ImGui::TextUnformatted(info.name.data(), info.name.data() + info.name.size());
                        ImGui::SameLine();
                        if (ImGui::Selectable(std::string(info.type_signature).c_str()))
                        {
                            std::unique_ptr<Node> node = std::make_unique<NativeFuncCall>(0, *type, method);
                            accept(document_.insertNode(node));
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndMenu();
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Script abilities", !metadata.abilities.nodes().empty()))
        {
            for (const auto &ability : metadata.abilities.nodes())
            {
                ImGui::PushID(&ability);
                const auto contract = ability.contract.name();
                ImGui::TextUnformatted(contract.data(), contract.data() + contract.size());
                ImGui::SameLine();
                if (ImGui::Selectable(std::string(ability.method.name()).c_str()))
                {
                    std::unique_ptr<Node> node = std::make_unique<ScriptAbilityNode>(0, ability);
                    accept(document_.insertNode(node));
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Script events", !metadata.events.empty()))
        {
            for (const auto &event : metadata.events)
            {
                ImGui::PushID(&event);
                ImGui::TextUnformatted(event.system_name.c_str());
                ImGui::SameLine();
                if (ImGui::Selectable(event.event_name.c_str()))
                {
                    std::unique_ptr<Node> node = std::make_unique<ScriptEventAwaitNode>(0, event);
                    accept(document_.insertNode(node));
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
    }

    static std::span<const lux::meta::RefType *const> scalarTypes()
    {
        static const std::array values{
            lux::meta::builtin_ref_type_ptr<bool>(),          lux::meta::builtin_ref_type_ptr<std::int32_t>(),
            lux::meta::builtin_ref_type_ptr<std::uint32_t>(), lux::meta::builtin_ref_type_ptr<std::int64_t>(),
            lux::meta::builtin_ref_type_ptr<std::uint64_t>(), lux::meta::builtin_ref_type_ptr<float>(),
            lux::meta::builtin_ref_type_ptr<double>()};
        return values;
    }
    static Scalar zero(const lux::meta::RefType &type)
    {
        auto value = lux::meta::RuntimeObject::defaultOf(&type);
        if (!value)
        {
            return std::monostate{};
        }
        auto literal = lux::flowforge::captureFlowLiteral(value);
        return literal ? scalar(*literal) : Scalar{std::monostate{}};
    }
    static bool scalarField(Scalar &scalar)
    {
        return std::visit(
            [](auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, bool>)
                {
                    return ImGui::Checkbox("##value", &value);
                }
                else if constexpr (!std::is_same_v<T, std::monostate>)
                {
                    constexpr auto type = std::is_same_v<T, double> ? ImGuiDataType_Double
                                          : std::is_signed_v<T>     ? ImGuiDataType_S64
                                                                    : ImGuiDataType_U64;
                    return ImGui::InputScalar("##value", type, &value);
                }
                else
                {
                    ImGui::TextDisabled("Default value");
                    return false;
                }
            },
            scalar);
    }
    static lux::flowforge::FlowSourceLiteral scalarLiteral(const Scalar &scalar)
    {
        return std::visit(
            [](const auto &value) -> lux::flowforge::FlowSourceLiteral {
                using T = std::decay_t<decltype(value)>;
                using K = lux::flowforge::EFlowLiteralKind;
                if constexpr (std::is_same_v<T, bool>)
                {
                    return {K::BOOLEAN, value ? "true" : "false"};
                }
                else if constexpr (std::is_same_v<T, std::monostate>)
                {
                    return {};
                }
                else
                {
                    char bytes[96];
                    const auto result = std::to_chars(bytes, bytes + sizeof(bytes), value);
                    constexpr auto kind = std::is_same_v<T, double> ? K::REAL
                                          : std::is_signed_v<T>     ? K::SIGNED
                                                                    : K::UNSIGNED;
                    return {kind, {bytes, result.ptr}};
                }
            },
            scalar);
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
    static bool drawArguments(const char *label, std::vector<lux::flowforge::FlowSourceArgument> &arguments)
    {
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        bool changed{};
        for (std::size_t index{}; index < arguments.size(); ++index)
        {
            ImGui::PushID(static_cast<int>(index));
            auto &argument = arguments[index];
            ImGui::SetNextItemWidth(140);
            changed |= ImGui::InputText("##name", &argument.name);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130);
            if (ImGui::BeginCombo("##type", argument.type.c_str()))
            {
                for (const auto *type : scalarTypes())
                {
                    if (ImGui::Selectable(type->name.data(), argument.type == type->name))
                    {
                        argument.type = type->name;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Add"))
        {
            arguments.push_back({"value" + std::to_string(arguments.size() + 1),
                                 std::string(lux::meta::builtin_ref_type_ptr<double>()->name)});
            changed = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(arguments.empty());
        if (ImGui::SmallButton("Remove last"))
        {
            arguments.pop_back();
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        return changed;
    }
    void drawFunctions()
    {
        using O = lux::flowforge::ENodeOperation;
        if (!ImGui::CollapsingHeader("Functions and events"))
        {
            return;
        }
        const auto current = document_.historyView()->history.current;
        if (function_index_state_ != current)
        {
            function_entries_.clear();
            for (const auto &node : document_.nodes())
            {
                const auto operation = document_.nodeOperation(node.id);
                if (operation == O::FUNC_DEF_START || operation == O::ON_EVENT)
                {
                    function_entries_.push_back(node.id);
                }
            }
            std::erase_if(function_drafts_, [&](const auto &entry) {
                return std::ranges::find(function_entries_, entry.first) == function_entries_.end();
            });
            function_index_state_ = current;
        }
        ImGui::BeginDisabled(!document_.project().writable());
        for (const auto id : function_entries_)
        {
            ImGui::PushID(reinterpret_cast<void *>(static_cast<std::uintptr_t>(id.value)));
            if (ImGui::TreeNode("entry", "%.*s", static_cast<int>(document_.nodeName(id).size()),
                                document_.nodeName(id).data()))
            {
                auto found = function_drafts_.find(id);
                if (found == function_drafts_.end() || (!found->second.dirty && found->second.base != current))
                {
                    auto captured = document_.captureNode(id);
                    if (accept(captured))
                    {
                        found =
                            function_drafts_.insert_or_assign(id, FunctionDraft{std::move(*captured), current}).first;
                    }
                }
                if (found != function_drafts_.end())
                {
                    auto &draft = found->second;
                    draft.dirty |= ImGui::InputText("Name", &draft.source.name);
                    auto &signature = std::get<lux::flowforge::FlowSourceSignature>(draft.source.parameters);
                    draft.dirty |= drawArguments("Arguments", signature.arguments);
                    const bool function = draft.source.operation == O::FUNC_DEF_START;
                    if (function)
                    {
                        draft.dirty |= drawArguments("Results", signature.results);
                    }
                    if (draft.dirty && draft.base != current)
                    {
                        ImGui::TextDisabled("Document changed. Revert this draft before applying.");
                    }
                    ImGui::BeginDisabled(!draft.dirty);
                    if (ImGui::Button("Apply signature"))
                    {
                        if (accept(document_.setFunctionSignature(draft.base, id, draft.source.name, signature)))
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
                            accept(document_.insertFunctionUse(id, false));
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Create return"))
                        {
                            accept(document_.insertFunctionUse(id, true));
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
        const auto current = document_.historyView()->history.current;
        if (!exports_draft_.dirty && exports_draft_.base != current)
        {
            exports_draft_.values.assign(document_.exports().begin(), document_.exports().end());
            exports_draft_.base = current;
        }
        auto &draft = exports_draft_;
        ImGui::BeginDisabled(!document_.project().writable());
        for (std::size_t index{}; index < draft.values.size(); ++index)
        {
            auto &value = draft.values[index];
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::BeginCombo("Event", document_.nodeName(value.entry_node_id).data()))
            {
                for (const auto &node : document_.nodes())
                {
                    if (document_.nodeOperation(node.id) == lux::flowforge::ENodeOperation::ON_EVENT &&
                        ImGui::Selectable(document_.nodeName(node.id).data(), node.id == value.entry_node_id))
                    {
                        value.entry_node_id = node.id;
                        draft.dirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            draft.dirty |= ImGui::InputScalar("Export identity", ImGuiDataType_U64, &value.id.value);
            draft.dirty |= ImGui::InputScalar("Symbol", ImGuiDataType_U64, &value.symbol, nullptr, nullptr, "%016llX",
                                              ImGuiInputTextFlags_CharsHexadecimal);
            for (std::size_t hint_index{}; hint_index < value.binding_hints.size(); ++hint_index)
            {
                auto &hint = value.binding_hints[hint_index];
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
            for (const auto &value : draft.values)
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
            if (accept(document_.setExports(draft.values)))
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
            std::erase_if(variable_drafts_, [this](const auto &entry) {
                return std::ranges::find(document_.variables(), entry.first,
                                         &lux::flowforge::FlowGraph::GraphVariable::id) == document_.variables().end();
            });
        }

        if (!ImGui::CollapsingHeader("Variables"))
        {
            return;
        }
        ImGui::BeginDisabled(!document_.project().writable());
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("New variable", &variable_name_);
        ImGui::SameLine();
        const auto types = scalarTypes();
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
            if (accept(document_.addVariable(variable_name_, types[variable_type_]->name,
                                             scalarLiteral(zero(*types[variable_type_])))))
            {
                variable_name_.clear();
            }
        }
        bool mutated{};
        for (const auto &variable : document_.variables())
        {
            auto [position, inserted] = variable_drafts_.try_emplace(variable.id);
            auto &draft = position->second;
            if (inserted || (source_changed_ && !draft.dirty))
            {
                auto captured = lux::flowforge::captureFlowVariable(variable);
                if (!accept(captured))
                {
                    continue;
                }
                draft.source = std::move(*captured);
                draft.value = scalar(draft.source.value);
            }
            ImGui::PushID(reinterpret_cast<const void *>(static_cast<std::uintptr_t>(variable.id)));
            bool changed{};
            ImGui::SetNextItemWidth(160);
            changed |= ImGui::InputText("##name", &draft.source.name);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            if (ImGui::BeginCombo("##type", draft.source.type.c_str()))
            {
                for (const auto *type : types)
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
            changed |= scalarField(draft.value);
            if (changed && !draft.dirty)
            {
                draft.base = document_.historyView()->history.current;
                draft.dirty = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!draft.dirty);
            if (ImGui::SmallButton("Apply") && unchanged(draft.base))
            {
                if (draft.value.index() != 0)
                {
                    draft.source.value = scalarLiteral(draft.value);
                }
                if (accept(document_.setVariable(draft.source)))
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
                    draft.value = scalar(draft.source.value);
                    draft.dirty = false;
                }

                mutated = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Get"))
            {
                std::unique_ptr<lux::flowforge::Node> node = std::make_unique<lux::flowforge::GetVariableNode>(
                    variable.id, lux::flowforge::DataPinInfo{variable.name, variable.type});
                accept(document_.insertNode(node));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Set"))
            {
                std::unique_ptr<lux::flowforge::Node> node = std::make_unique<lux::flowforge::SetVariableNode>(
                    variable.id, lux::flowforge::DataPinInfo{variable.name, variable.type});
                accept(document_.insertNode(node));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove"))
            {
                mutated = accept(document_.removeVariable(variable.id));
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
        auto &field = found->second;
        ImGui::PushID(reinterpret_cast<const void *>(static_cast<std::uintptr_t>(pin.value)));
        ImGui::SetNextItemWidth(160);
        const bool changed = scalarField(field.value);
        if (ImGui::IsItemActivated())
        {
            field.base = document_.historyView()->history.current;
            field.active = true;
        }
        const bool commit = field.value.index() == 1 ? changed : ImGui::IsItemDeactivatedAfterEdit();
        if (commit && unchanged(field.base))
        {
            auto literal = scalarLiteral(field.value);
            accept(document_.setPinLiteral(pin, literal));
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
    FlowForgePane(flowforge::FlowForgeEditor &document, std::string id)
        : DocumentPane(document, std::move(id), "FlowForge"), canvas_(createCanvas()), name_(document.summary().title),
          content_connection_(document.observeScoped<flowforge::FlowForgeEditor::contentChanged>(
              [this](editing::Revision) noexcept { source_changed_ = true; }))
    {
    }

    void poll(PollBudget &budget) override
    {
        publication_.poll(document_);
        DocumentPane::poll(budget);
    }

  private:
    PublicationControls publication_;

    template <class Result> bool accept(const Result &result)
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
            error_ = result.error().domain + ":" + std::to_string(result.error().reason) + " " + result.error().message;
        }
        return false;
    }
    bool unchanged(editing::StateId state)
    {
        const auto history = document_.historyView();
        if (!history || history->history.current != state)
        {
            error_ = "The graph changed during this gesture. Start the edit again.";
            return false;
        }
        return true;
    }
    void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context) override
    {
        context.activateContext(lux::ui::UiContextIdView{id()});
        if (!canvas_)
        {
            frame.text("Node canvas could not be created");
            return;
        }
        CanvasScope scope(canvas_.get());
        const auto history = document_.historyView();
        if (!history)
        {
            accept(history);
            return;
        }
        ImGui::BeginDisabled(!document_.project().writable());
        if (source_changed_ && !name_active_)
        {
            name_ = document_.summary().title;
        }
        ImGui::SetNextItemWidth(240);
        ImGui::InputText("Name", &name_);
        if (ImGui::IsItemActivated())
        {
            name_base_ = history->history.current;
            name_active_ = true;
        }
        if (ImGui::IsItemDeactivated())
        {
            if (ImGui::IsItemDeactivatedAfterEdit() && unchanged(name_base_))
            {
                accept(document_.rename(name_));
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
            constexpr std::array kinds{O::ON_EVENT, O::BRANCH, O::SEQUENCE,       O::FOR_LOOP, O::WHILE_LOOP,
                                       O::BREAK,    O::RETURN, O::FUNC_DEF_START, O::ADD,      O::SUBTRACT,
                                       O::MULTIPLY, O::DIVIDE, O::CMP_EQ,         O::CMP_NE,   O::CMP_LT,
                                       O::CMP_LE,   O::CMP_GT, O::CMP_GE};
            for (const auto kind : kinds)
            {
                if (ImGui::Selectable(lux::flowforge::toString(kind)))
                {
                    auto node = createNode(kind);
                    accept(document_.insertNode(node));
                }
            }
            drawRegisteredNodes();
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Compile"))
        {
            bool ready = true;
            if (compile_.serial)
            {
                ready = accept(document_.acknowledgeCompile(compile_));
            }
            if (ready)
            {
                auto request = document_.requestCompile(std::filesystem::u8path(linker_));
                if (accept(request))
                {
                    compile_ = *request;
                }
            }
        }
        if (compile_.serial)
        {
            auto status = document_.compileStatus(compile_);
            if (status)
            {
                if (std::holds_alternative<flowforge::FlowCompilePending>(*status))
                {
                    frame.textMuted("Compiling...");
                }
                else if (const auto *success = std::get_if<flowforge::FlowCompileSucceeded>(&*status))
                {
                    frame.textMuted(success->current ? "Compilation succeeded"
                                                     : "Compiled result belongs to an earlier edit");
                    ImGui::BeginDisabled(!success->current || !document_.project().writable() ||
                                         !document_.saveRequests().empty());
                    if (ImGui::SmallButton("Save source and publish asset"))
                    {
                        const auto publication = document_.requestPublish(compile_, "desktop");
                        if (accept(publication))
                        {
                            publication_.track(*publication);
                        }
                    }
                    ImGui::EndDisabled();
                }
                else
                {
                    const auto &failure = std::get<flowforge::FlowCompileFailed>(*status).failure;
                    frame.text(failure.message);
                    if (std::get<flowforge::FlowCompileFailed>(*status).retryable && ImGui::Button("Retry link"))
                    {
                        accept(document_.retryLink(compile_, std::filesystem::u8path(linker_)));
                    }
                }
            }
        }
        publication_.draw(document_, frame);
        drawVariables();
        drawFunctions();
        drawExports();
        if (ImGui::TreeNode("Compiler settings"))
        {
            ImGui::InputText("Linker", &linker_);
            frame.textMuted("Leave empty to use the configured system linker");
            ImGui::TreePop();
        }
        if (source_changed_)
        {
            pin_groups_.clear();
            for (const auto &pin : document_.pins())
            {
                pin_groups_[pin.owner].push_back(pin);
                if (pin.direction == lux::graph::EPinDirection::INPUT && !document_.pinType(pin.id).empty())
                {
                    auto &field = literals_[pin.id];
                    if (!field.active)
                    {
                        const auto value = document_.pinLiteral(pin.id);
                        field.value = value ? scalar(*value) : Scalar{std::monostate{}};
                    }
                }
            }
            std::erase_if(literals_, [&](const auto &item) { return document_.pinType(item.first).empty(); });
        }
        if (!error_.empty())
        {
            frame.text(error_);
        }
        source_changed_ = false;
        drawCanvas();
    }
    void drawCanvas()
    {
        const bool writable = document_.project().writable();
        canvas::Begin("FlowForge graph");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            move_base_ = document_.historyView()->history.current;
        }
        std::size_t ordinal{};
        for (const auto &record : document_.nodes())
        {
            const auto layout = document_.nodeLayout(record.id);
            auto [position, inserted] = positions_.try_emplace(record.id);
            if (inserted || position->second.author != layout)
            {
                position->second.author = layout;
                position->second.display = layout.placed ? layout
                                                         : lux::graph::GraphNodeLayout{float(ordinal % 4) * 240,
                                                                                       float(ordinal / 4) * 280, true};
                canvas::SetNodePosition(canvas_ids_.node(record.id.value),
                                        {position->second.display.x, position->second.display.y});
            }
            ++ordinal;
            canvas::BeginNode(canvas_ids_.node(record.id.value));
            const auto name = document_.nodeName(record.id);
            if (name.empty())
            {
                ImGui::TextUnformatted(lux::flowforge::toString(document_.nodeOperation(record.id)));
            }
            else
            {
                ImGui::TextUnformatted(name.data(), name.data() + name.size());
            }
            for (const auto &pin : pin_groups_[record.id])
            {
                const bool input = pin.direction == lux::graph::EPinDirection::INPUT;
                canvas::BeginPin(canvas_ids_.pin(pin.id.value),
                                 input ? canvas::PinKind::Input : canvas::PinKind::Output);
                const auto label = document_.pinName(pin.id);
                const auto type = document_.pinType(pin.id);
                ImGui::Text("%s %.*s %.*s", input ? "<" : ">", int(label.size()), label.data(), int(type.size()),
                            type.data());
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
        const auto links = document_.links();
        for (const auto &link : links)
        {
            const auto key = std::pair{link.from.value, link.to.value};
            auto [found, inserted] = links_.try_emplace(key, next_link_);
            if (inserted)
            {
                ++next_link_;
            }
            canvas::Link(canvas_ids_.link(found->second), canvas_ids_.pin(link.from.value),
                         canvas_ids_.pin(link.to.value));
        }
        lux::graph::LinkRecord created;
        if (writable && canvas::BeginCreate())
        {
            canvas::PinId first, second;
            if (canvas::QueryNewLink(&first, &second) && first && second && canvas::AcceptNewItem())
            {
                created = {{canvas_ids_.source(first)}, {canvas_ids_.source(second)}};
                const auto pins = document_.pins();
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
                const auto found = std::ranges::find_if(
                    links_, [&](const auto &value) { return value.second == canvas_ids_.source(link); });
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
            accept(document_.removeNodes(removed_nodes, removed_links));
        }
        if (created.from.valid() && created.to.valid())
        {
            accept(document_.connect(created.from, created.to));
        }
        if (writable && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            std::vector<lux::graph::GraphLayoutEntry> moved;
            for (auto &[node, position] : positions_)
            {
                if (document_.nodeOperation(node) == lux::flowforge::ENodeOperation::INVALID)
                {
                    continue;
                }
                const auto value = canvas::GetNodePosition(canvas_ids_.node(node.value));
                if (value.x != position.display.x || value.y != position.display.y)
                {
                    moved.push_back({node, {value.x, value.y, true}});
                }
            }
            if (!moved.empty() && (!unchanged(move_base_) || !accept(document_.moveNodes(moved))))
            {
                for (const auto &item : moved)
                {
                    const auto &position = positions_.at(item.node).display;
                    canvas::SetNodePosition(canvas_ids_.node(item.node.value), {position.x, position.y});
                }
            }
        }
        std::erase_if(positions_, [&](const auto &item) {
            return document_.nodeOperation(item.first) == lux::flowforge::ENodeOperation::INVALID;
        });
        std::erase_if(links_, [&](const auto &item) {
            return std::ranges::find(document_.links(),
                                     lux::graph::LinkRecord{{item.first.first}, {item.first.second}}) ==
                   document_.links().end();
        });
    }
    NodeCanvasIds canvas_ids_;
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
    flowforge::FlowCompileId compile_;
    std::string name_, error_, linker_;
    editing::StateId name_base_, move_base_;
    bool source_changed_{true}, name_active_{};
    object::ScopedConnection content_connection_;
};
} // namespace

GuiDocumentProvider flowForgeDocumentProvider(lux::flowforge::FlowSourceEnvironment environment)
{
    return {std::string(flowforge::kFlowForgeDocumentType),
            [](const ProjectAssetEntry &asset) { return asset.kind == EProjectAssetKind::FLOW_GRAPH; },
            [environment = std::move(environment)](process::ExecutionRuntime &runtime,
                                                   lux::render::RenderRuntime &) -> EditorResult<DocumentRegistration> {
                return DocumentRegistration{
                    std::string(flowforge::kFlowForgeDocumentType),
                    [&runtime, environment](Project &project, const OpenDocumentRequest &request) {
                        return flowforge::openFlowForgeDocument(project, request, runtime, environment);
                    }};
            },
            [](DocumentEditor &base, ui::UIRenderSystem &ui, lux::render::RenderRuntime &,
               process::ExecutionRuntime &) -> EditorResult<void> {
                auto *document = dynamic_cast<flowforge::FlowForgeEditor *>(&base);
                if (!document)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.gui"});
                }
                const auto id = "flowforge-" + std::to_string(document->historyId().value);
                for (const auto &view : document->views())
                {
                    if (view->id() == id)
                    {
                        if (view->closeStatus().state != ECloseState::OPEN)
                        {
                            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.views"});
                        }
                        return {};
                    }
                }
                auto pane = std::make_unique<FlowForgePane>(*document, id);
                auto attached = pane->attach(ui);
                if (!attached)
                {
                    pane->requestClose();
                    return attached;
                }
                std::vector<std::unique_ptr<DocumentView>> batch;
                batch.push_back(std::move(pane));
                auto adopted = document->addViews(batch);
                if (!adopted)
                {
                    for (const auto &view : batch)
                    {
                        view->requestClose();
                    }
                }
                return adopted;
            }};
}
} // namespace lux::editor::gui
