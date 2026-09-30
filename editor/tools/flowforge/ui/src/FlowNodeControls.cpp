#include <lux/engine/editor/flowforge/FlowNodeControls.hpp>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <array>
#include <charconv>
namespace lux::editor::flowforge
{
    std::unique_ptr<lux::flowforge::Node> makeFlowNode(lux::flowforge::ENodeOperation operation)
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
    VFlowScalar flowScalar(const lux::flowforge::FlowSourceLiteral& literal)
    {
        using K = lux::flowforge::EFlowLiteralKind;
        if (literal.kind == K::BOOLEAN)
        {
            return literal.value == "true";
        }
        const auto parse = [&]<class T>() -> VFlowScalar {
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
    std::span<const lux::meta::RefType* const> flowScalarTypes()
    {
        static const std::array values{
            lux::meta::builtin_ref_type_ptr<bool>(),
            lux::meta::builtin_ref_type_ptr<std::int32_t>(),
            lux::meta::builtin_ref_type_ptr<std::uint32_t>(),
            lux::meta::builtin_ref_type_ptr<std::int64_t>(),
            lux::meta::builtin_ref_type_ptr<std::uint64_t>(),
            lux::meta::builtin_ref_type_ptr<float>(),
            lux::meta::builtin_ref_type_ptr<double>()
        };
        return values;
    }
    bool editFlowScalar(VFlowScalar& scalar)
    {
        return std::visit(
            [](auto& value) {
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
            scalar
        );
    }
    lux::flowforge::FlowSourceLiteral flowScalarLiteral(const VFlowScalar& scalar)
    {
        return std::visit(
            [](const auto& value) -> lux::flowforge::FlowSourceLiteral {
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
            scalar
        );
    }
    bool editFlowArguments(const char* label, std::vector<lux::flowforge::FlowSourceArgument>& arguments)
    {
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        bool changed{};
        for (std::size_t index{}; index < arguments.size(); ++index)
        {
            ImGui::PushID(static_cast<int>(index));
            auto& argument = arguments[index];
            ImGui::SetNextItemWidth(140);
            changed |= ImGui::InputText("##name", &argument.name);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130);
            if (ImGui::BeginCombo("##type", argument.type.c_str()))
            {
                for (const auto* type : flowScalarTypes())
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
            arguments.push_back(
                {"value" + std::to_string(arguments.size() + 1),
                 std::string(lux::meta::builtin_ref_type_ptr<double>()->name)}
            );
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
    std::unique_ptr<lux::flowforge::Node> chooseRegisteredFlowNode(const lux::flowforge::FlowSourceEnvironment& metadata
    )
    {
        using namespace lux::flowforge;
        std::unique_ptr<Node> selected;
        if (ImGui::BeginMenu("Native functions", !metadata.functions.empty()))
        {
            for (const auto* function : metadata.functions)
            {
                ImGui::PushID(function);
                const auto& info = function->invokable;
                ImGui::TextUnformatted(info.full_name.data(), info.full_name.data() + info.full_name.size());
                ImGui::SameLine();
                if (ImGui::Selectable(std::string(info.type_signature).c_str()))
                {
                    std::unique_ptr<Node> node = std::make_unique<NativeFuncCall>(0, *function);
                    selected = std::move(node);
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Classes", !metadata.classes.empty()))
        {
            for (const auto* type : metadata.classes)
            {
                ImGui::PushID(type);
                if (ImGui::BeginMenu(std::string(type->full_name).c_str()))
                {
                    for (const auto& field : type->fields)
                    {
                        if (field.visibility != lux::meta::EVisibility::PUBLIC || field.is_volatile)
                        {
                            continue;
                        }
                        ImGui::PushID(&field);
                        ImGui::TextUnformatted(field.name.data(), field.name.data() + field.name.size());
                        ImGui::SameLine();
                        if (ImGui::Selectable("Read"))
                        {
                            std::unique_ptr<Node> node = std::make_unique<GetFieldNode>(0, *type, field);
                            selected = std::move(node);
                        }
                        if (!field.is_const && ImGui::Selectable("Write"))
                        {
                            std::unique_ptr<Node> node = std::make_unique<SetFieldNode>(0, *type, field);
                            selected = std::move(node);
                        }
                        ImGui::PopID();
                    }
                    for (const auto& method : type->methods)
                    {
                        if (method.visibility != lux::meta::EVisibility::PUBLIC)
                        {
                            continue;
                        }
                        ImGui::PushID(&method);
                        const auto& info = method.invokable;
                        ImGui::TextUnformatted(info.name.data(), info.name.data() + info.name.size());
                        ImGui::SameLine();
                        if (ImGui::Selectable(std::string(info.type_signature).c_str()))
                        {
                            std::unique_ptr<Node> node = std::make_unique<NativeFuncCall>(0, *type, method);
                            selected = std::move(node);
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
            for (const auto& ability : metadata.abilities.nodes())
            {
                ImGui::PushID(&ability);
                const auto contract = ability.contract.name();
                ImGui::TextUnformatted(contract.data(), contract.data() + contract.size());
                ImGui::SameLine();
                if (ImGui::Selectable(std::string(ability.method.name()).c_str()))
                {
                    std::unique_ptr<Node> node = std::make_unique<ScriptAbilityNode>(0, ability);
                    selected = std::move(node);
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Script events", !metadata.events.empty()))
        {
            for (const auto& event : metadata.events)
            {
                ImGui::PushID(&event);
                ImGui::TextUnformatted(event.system_name.c_str());
                ImGui::SameLine();
                if (ImGui::Selectable(event.event_name.c_str()))
                {
                    std::unique_ptr<Node> node = std::make_unique<ScriptEventAwaitNode>(0, event);
                    selected = std::move(node);
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        return selected;
    }
}
