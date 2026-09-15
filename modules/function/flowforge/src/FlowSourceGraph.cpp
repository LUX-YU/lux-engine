#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <lux/engine/meta/MetaCompat.hpp>
#include <map>
#include <set>

namespace lux::flowforge
{
namespace
{
auto fail(EFlowSourceError code, std::string field = {}, NodeId node = {}, PinId pin = {}) noexcept
{
    return lux::cxx::unexpected(FlowSourceFailure{code, std::move(field), node, pin});
}
template <class Fn> bool visitScalar(const lux::meta::RefType &type, Fn &&fn)
{
    if (type.qtype.qual != static_cast<unsigned>(lux::meta::ETypeQual::Value))
    {
        return false;
    }
    using B = lux::meta::EBaseType;
    switch (static_cast<B>(type.qtype.base))
    {
    case B::Bool:
        return type.size == sizeof(bool) && fn.template operator()<bool>();
    case B::Int8:
        return type.size == 1 && fn.template operator()<std::int8_t>();
    case B::Uint8:
        return type.size == 1 && fn.template operator()<std::uint8_t>();
    case B::Int16:
        return type.size == 2 && fn.template operator()<std::int16_t>();
    case B::Uint16:
        return type.size == 2 && fn.template operator()<std::uint16_t>();
    case B::Int32:
        return type.size == 4 && fn.template operator()<std::int32_t>();
    case B::Uint32:
        return type.size == 4 && fn.template operator()<std::uint32_t>();
    case B::Int64:
        return type.size == 8 && fn.template operator()<std::int64_t>();
    case B::Uint64:
        return type.size == 8 && fn.template operator()<std::uint64_t>();
    case B::Float:
        return type.size == 4 && fn.template operator()<float>();
    case B::Double:
        return type.size == 8 && fn.template operator()<double>();
    default:
        return false;
    }
}
FlowSourceResult<FlowSourceLiteral> captureLiteral(const lux::meta::RuntimeObject &object) noexcept
{
    if (!object.isValid())
    {
        return FlowSourceLiteral{};
    }
    FlowSourceLiteral literal;
    const bool scalar = visitScalar(*object.type(), [&]<class T>() {
        T value{};
        std::memcpy(&value, object.data(), sizeof(T));
        if constexpr (std::is_same_v<T, bool>)
        {
            literal = {EFlowLiteralKind::BOOLEAN, value ? "true" : "false"};
        }
        else
        {
            if constexpr (std::is_floating_point_v<T>)
            {
                if (!std::isfinite(value))
                {
                    return false;
                }
                literal.kind = EFlowLiteralKind::REAL;
            }
            else
            {
                literal.kind = std::is_signed_v<T> ? EFlowLiteralKind::SIGNED : EFlowLiteralKind::UNSIGNED;
            }
            char buffer[96];
            const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
            if (result.ec != std::errc{})
            {
                return false;
            }
            literal.value.assign(buffer, result.ptr);
        }
        return true;
    });
    if (scalar)
    {
        return literal;
    }
    // Null pointer/zero trivial object only. Never serialize a process address or opaque object bytes.
    const auto *bytes = static_cast<const std::byte *>(object.data());
    if (std::all_of(bytes, bytes + object.type()->size, [](std::byte value) { return value == std::byte{}; }))
    {
        return FlowSourceLiteral{EFlowLiteralKind::ZERO, {}};
    }
    return fail(EFlowSourceError::UNSUPPORTED_LITERAL, std::string(object.type()->name));
}
FlowSourceResult<lux::meta::RuntimeObject> materializeLiteral(const FlowSourceLiteral &literal,
                                                              const lux::meta::RefType &type) noexcept
{
    if (literal.kind == EFlowLiteralKind::NONE)
    {
        return lux::meta::RuntimeObject{};
    }
    auto result = lux::meta::RuntimeObject::defaultOf(&type);
    if (!result.isValid())
    {
        return fail(EFlowSourceError::UNSUPPORTED_LITERAL, std::string(type.name));
    }
    if (literal.kind == EFlowLiteralKind::ZERO)
    {
        return result;
    }
    const bool valid = visitScalar(type, [&]<class T>() {
        T value{};
        if constexpr (std::is_same_v<T, bool>)
        {
            if (literal.kind != EFlowLiteralKind::BOOLEAN)
            {
                return false;
            }
            value = literal.value == "true";
        }
        else
        {
            constexpr auto expected = std::is_floating_point_v<T> ? EFlowLiteralKind::REAL
                                      : std::is_signed_v<T>       ? EFlowLiteralKind::SIGNED
                                                                  : EFlowLiteralKind::UNSIGNED;
            if (literal.kind != expected)
            {
                return false;
            }
            const auto begin = literal.value.data(), end = begin + literal.value.size();
            const auto parsed = std::from_chars(begin, end, value);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
            {
                return false;
            }
            if constexpr (std::is_floating_point_v<T>)
            {
                if (!std::isfinite(value))
                {
                    return false;
                }
            }
        }
        std::memcpy(result.data(), &value, sizeof(T));
        return true;
    });
    if (!valid)
    {
        return fail(EFlowSourceError::UNSUPPORTED_LITERAL, std::string(type.name));
    }
    return result;
}
const lux::meta::RefType *pinType(const Pin &pin) noexcept
{
    if (pin.kind() == EPinKind::DATA_IN)
    {
        return static_cast<const DataInPin &>(pin).info().type;
    }
    if (pin.kind() == EPinKind::DATA_OUT)
    {
        return static_cast<const DataOutPin &>(pin).info().type;
    }
    return nullptr;
}
bool binary(ENodeOperation operation) noexcept
{
    return (operation >= ENodeOperation::ADD && operation <= ENodeOperation::LOGICAL_OR) ||
           (operation >= ENodeOperation::CMP_EQ && operation <= ENodeOperation::CMP_GE);
}
bool unary(ENodeOperation operation) noexcept
{
    return operation == ENodeOperation::NEGATE || operation == ENodeOperation::LOGICAL_NOT;
}
std::vector<FlowSourceArgument> captureArguments(const std::vector<FuncArgInfo> &values)
{
    std::vector<FlowSourceArgument> result;
    for (const auto &value : values)
    {
        result.push_back({value.name, value.type ? std::string(value.type->name) : std::string{}});
    }
    return result;
}
const lux::meta::RefType *findType(std::string_view name, const FlowSourceEnvironment &environment) noexcept
{
    const lux::meta::RefType *result{};
    const auto builtin = [&]<class... T>() {
        ((name == lux::meta::builtin_ref_type_ptr<T>()->name ? result = lux::meta::builtin_ref_type_ptr<T>() : result),
         ...);
    };
    builtin.template operator()<void, bool, char, signed char, unsigned char, short, unsigned short, int, unsigned int,
                                long, unsigned long, long long, unsigned long long, float, double, void *>();
    if (result)
    {
        return result;
    }
    for (const auto *type : environment.types)
    {
        if (type && type->name == name)
        {
            return type;
        }
    }
    for (const auto *type : environment.classes)
    {
        if (type && type->type.name == name)
        {
            return &type->type;
        }
    }
    return nullptr;
}
const lux::meta::RefClass *findClass(std::string_view name, const FlowSourceEnvironment &environment) noexcept
{
    for (const auto *type : environment.classes)
    {
        if (type && type->full_name == name)
        {
            return type;
        }
    }
    return nullptr;
}
FlowSourceResult<std::vector<FuncArgInfo>> materializeArguments(const std::vector<FlowSourceArgument> &values,
                                                                const FlowSourceEnvironment &environment) noexcept
{
    std::vector<FuncArgInfo> result;
    for (const auto &value : values)
    {
        const auto *type = findType(value.type, environment);
        if (!type)
        {
            return fail(EFlowSourceError::UNKNOWN_TYPE, value.type);
        }
        result.push_back({type, value.name});
    }
    return result;
}
bool signatureMatches(const lux::meta::RefInvokable &info, const FlowSourceNativeCall &source) noexcept
{
    const bool mismatch = info.full_name != source.member || info.type_signature != source.signature ||
                          info.parameters.size() != source.parameters.arguments.size();
    if (mismatch)
    {
        return false;
    }
    for (std::size_t i{}; i < info.parameters.size(); ++i)
    {
        if (info.parameters[i].type.name != source.parameters.arguments[i].type)
        {
            return false;
        }
    }
    return source.parameters.results.size() == 1 && info.return_type.name == source.parameters.results.front().type;
}
FlowSourceResult<std::unique_ptr<Node>> makeNode(const FlowSourceNode &source, FlowGraph &graph,
                                                 const FlowSourceEnvironment &environment) noexcept
{
    const auto id = source.id.value;
    const auto operation = source.operation;
    if (binary(operation) || unary(operation))
    {
        const auto *type = findType(std::get<FlowSourceType>(source.parameters).name, environment);
        if (!type)
        {
            return fail(EFlowSourceError::UNKNOWN_TYPE, std::get<FlowSourceType>(source.parameters).name, source.id);
        }
        if (binary(operation))
        {
            return std::unique_ptr<Node>(std::make_unique<BinaryOpNode>(id, operation, type));
        }
        return std::unique_ptr<Node>(std::make_unique<UnaryOpNode>(id, operation, type));
    }
    switch (operation)
    {
    case ENodeOperation::START:
        return std::unique_ptr<Node>(std::make_unique<StartNode>(id));
    case ENodeOperation::BRANCH:
        return std::unique_ptr<Node>(std::make_unique<BranchNode>(id));
    case ENodeOperation::FOR_LOOP:
        return std::unique_ptr<Node>(std::make_unique<ForLoopNode>(id));
    case ENodeOperation::WHILE_LOOP:
        return std::unique_ptr<Node>(std::make_unique<WhileLoopNode>(id));
    case ENodeOperation::RETURN:
        return std::unique_ptr<Node>(std::make_unique<ReturnNode>(id));
    case ENodeOperation::BREAK:
        return std::unique_ptr<Node>(std::make_unique<BreakNode>(id));
    case ENodeOperation::SEQUENCE: {
        if (source.outputs.empty())
        {
            return fail(EFlowSourceError::SCHEMA_MISMATCH, "sequence outputs", source.id);
        }
        auto node = std::make_unique<SequenceNode>(id);
        while (node->outPins().size() < source.outputs.size())
        {
            static_cast<void>(node->addExecOutPin());
        }
        return std::unique_ptr<Node>(std::move(node));
    }
    case ENodeOperation::FUNC_DEF_START:
    case ENodeOperation::ON_EVENT: {
        auto args = materializeArguments(std::get<FlowSourceSignature>(source.parameters).arguments, environment);
        if (!args)
        {
            return lux::cxx::unexpected(args.error());
        }
        if (operation == ENodeOperation::ON_EVENT)
        {
            return std::unique_ptr<Node>(std::make_unique<OnEventNode>(id, source.name, std::move(*args)));
        }
        auto results = materializeArguments(std::get<FlowSourceSignature>(source.parameters).results, environment);
        if (!results)
        {
            return lux::cxx::unexpected(results.error());
        }
        return std::unique_ptr<Node>(
            std::make_unique<FuncDefNode>(id, source.name, std::move(*args), std::move(*results)));
    }
    case ENodeOperation::FUNC_RETURN:
    case ENodeOperation::GRAPH_FUNC_CALL: {
        const auto *target = graph.findNodeById(NodeId{std::get<FlowSourceReference>(source.parameters).id});
        if (!target || target->operation() != ENodeOperation::FUNC_DEF_START)
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "function", source.id);
        }
        const auto &definition = static_cast<const FuncDefNode &>(*target);
        if (operation == ENodeOperation::FUNC_RETURN)
        {
            return std::unique_ptr<Node>(std::make_unique<FuncReturnNode>(id, definition));
        }
        return std::unique_ptr<Node>(std::make_unique<GraphFuncCallNode>(id, definition));
    }
    case ENodeOperation::GET_VARIABLE:
    case ENodeOperation::SET_VARIABLE: {
        const auto *variable = graph.findVariable(std::get<FlowSourceReference>(source.parameters).id);
        if (!variable)
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "variable", source.id);
        }
        const DataPinInfo info{variable->name, variable->type};
        if (operation == ENodeOperation::GET_VARIABLE)
        {
            return std::unique_ptr<Node>(std::make_unique<GetVariableNode>(id, variable->id, info));
        }
        return std::unique_ptr<Node>(std::make_unique<SetVariableNode>(id, variable->id, info));
    }
    case ENodeOperation::GET_OBJECT:
    case ENodeOperation::SET_OBJECT: {
        const auto *type = findType(std::get<FlowSourceType>(source.parameters).name, environment);
        if (!type)
        {
            return fail(EFlowSourceError::UNKNOWN_TYPE, std::get<FlowSourceType>(source.parameters).name, source.id);
        }
        if (operation == ENodeOperation::GET_OBJECT)
        {
            return std::unique_ptr<Node>(std::make_unique<GetObjectNode>(id, *type));
        }
        return std::unique_ptr<Node>(std::make_unique<SetObjectNode>(id, *type));
    }
    case ENodeOperation::GET_FIELD:
    case ENodeOperation::SET_FIELD: {
        const auto &field_source = std::get<FlowSourceField>(source.parameters);
        const auto *owner = findClass(field_source.owner, environment);
        if (owner)
        {
            for (const auto &field : owner->fields)
            {
                if (field.name == field_source.member && field.type.name == field_source.type)
                {
                    if (operation == ENodeOperation::GET_FIELD)
                    {
                        return std::unique_ptr<Node>(std::make_unique<GetFieldNode>(id, *owner, field));
                    }
                    return std::unique_ptr<Node>(std::make_unique<SetFieldNode>(id, *owner, field));
                }
            }
        }
        return fail(EFlowSourceError::UNKNOWN_REFLECTION_MEMBER, field_source.owner + "::" + field_source.member,
                    source.id);
    }
    case ENodeOperation::NATIVE_FUNC_CALL: {
        const auto &call = std::get<FlowSourceNativeCall>(source.parameters);
        if (call.owner.empty())
        {
            for (const auto *function : environment.functions)
            {
                if (function && signatureMatches(function->invokable, call))
                {
                    return std::unique_ptr<Node>(std::make_unique<NativeFuncCall>(id, *function));
                }
            }
        }
        else if (const auto *owner = findClass(call.owner, environment))
        {
            for (const auto &method : owner->methods)
            {
                if (signatureMatches(method.invokable, call))
                {
                    return std::unique_ptr<Node>(std::make_unique<NativeFuncCall>(id, *owner, method));
                }
            }
        }
        return fail(EFlowSourceError::UNKNOWN_REFLECTION_MEMBER, call.member, source.id);
    }
    case ENodeOperation::SCRIPT_ABILITY_CALL: {
        const auto &ability = std::get<FlowSourceAbility>(source.parameters);
        const auto *description = environment.abilities.find(lux::script::ScriptApiContractIdView{ability.contract},
                                                             lux::script::ScriptApiMethodIdView{ability.method});
        if (!description)
        {
            return fail(EFlowSourceError::UNKNOWN_ABILITY, ability.method, source.id);
        }
        if (description->schema_version != ability.schema_version || description->schema_hash != ability.schema_hash)
        {
            return fail(EFlowSourceError::SCHEMA_MISMATCH, ability.method, source.id);
        }
        return std::unique_ptr<Node>(std::make_unique<ScriptAbilityNode>(id, *description));
    }
    case ENodeOperation::SCRIPT_EVENT_WAIT: {
        const auto &saved_event = std::get<lux::script::ScriptEventSourceDescription>(source.parameters);
        for (const auto &event : environment.events)
        {
            if (event.system_id == saved_event.system_id && event.event_id == saved_event.event_id)
            {
                if (event != saved_event)
                {
                    return fail(EFlowSourceError::SCHEMA_MISMATCH, "event", source.id);
                }
                return std::unique_ptr<Node>(std::make_unique<ScriptEventAwaitNode>(id, event));
            }
        }
        return fail(EFlowSourceError::SCHEMA_MISMATCH, "missing event", source.id);
    }
    default:
        return fail(EFlowSourceError::UNKNOWN_NODE_KIND, "operation", source.id);
    }
}
} // namespace

FlowSourceResult<void> validateFlowSourceEnvironment(const FlowSourceEnvironment &environment,
                                                     FlowSourceLimits limits) noexcept
{
    const bool too_many = environment.types.size() > limits.max_pins || environment.classes.size() > limits.max_nodes ||
                          environment.functions.size() > limits.max_nodes ||
                          environment.abilities.nodes().size() > limits.max_nodes ||
                          environment.events.size() > limits.max_nodes;
    if (too_many)
    {
        return fail(EFlowSourceError::LIMIT_EXCEEDED, "metadata");
    }
    std::map<std::string_view, const lux::meta::RefType *> types;
    for (const auto *type : environment.types)
    {
        if (!type || type->name.empty() || type->size == 0)
        {
            return fail(EFlowSourceError::UNKNOWN_TYPE, "metadata.type");
        }
        if (!types.emplace(type->name, type).second)
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "metadata.type.duplicate");
        }
    }
    std::set<std::string_view> classes;
    for (const auto *type : environment.classes)
    {
        if (!type || type->full_name.empty() || type->type.name.empty() || type->type.size == 0)
        {
            return fail(EFlowSourceError::UNKNOWN_TYPE, "metadata.class");
        }
        const auto [found, inserted] = types.emplace(type->type.name, &type->type);
        if ((!inserted && found->second != &type->type) || !classes.emplace(type->full_name).second)
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "metadata.class.duplicate");
        }
    }
    std::set<std::pair<std::string_view, std::string_view>> functions;
    for (const auto *function : environment.functions)
    {
        if (!function || function->invokable.full_name.empty() || function->invokable.type_signature.empty())
        {
            return fail(EFlowSourceError::UNKNOWN_REFLECTION_MEMBER, "metadata.function");
        }
        if (!functions.emplace(function->invokable.full_name, function->invokable.type_signature).second)
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "metadata.function.duplicate");
        }
    }
    ScriptAbilityNodeCatalog abilities;
    if (auto valid = abilities.add({environment.abilities.nodes()}); !valid)
    {
        return fail(EFlowSourceError::SCHEMA_MISMATCH, "metadata.ability");
    }
    std::set<std::pair<std::uint64_t, std::uint64_t>> events;
    for (const auto &event : environment.events)
    {
        if (!event.valid())
        {
            return fail(EFlowSourceError::SCHEMA_MISMATCH, "metadata.event");
        }
        if (!events.emplace(event.system_id, event.event_id).second)
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "metadata.event.duplicate");
        }
    }
    return {};
}

FlowSourceResult<std::vector<FuncArgInfo>> materializeFlowArguments(std::span<const FlowSourceArgument> values,
                                                                    const FlowSourceEnvironment &environment,
                                                                    FlowSourceLimits limits) noexcept
{
    if (values.size() > limits.max_pins)
    {
        return fail(EFlowSourceError::LIMIT_EXCEEDED, "signature");
    }
    std::vector<FuncArgInfo> result;
    result.reserve(values.size());
    for (const auto &value : values)
    {
        const bool invalid_name = value.name.empty() || value.name.size() > limits.max_string_bytes ||
                                  value.name.find('\0') != std::string::npos;
        const bool duplicate_name = std::ranges::find(result, value.name, &FuncArgInfo::name) != result.end();
        if (invalid_name || duplicate_name)
        {
            return fail(EFlowSourceError::INVALID_VALUE, "argument.name");
        }
        const auto *type = findType(value.type, environment);
        if (!type || !type->size)
        {
            return fail(EFlowSourceError::UNKNOWN_TYPE, value.type);
        }
        result.push_back({type, value.name});
    }
    return result;
}

FlowSourceResult<FlowSourceLiteral> captureFlowLiteral(const lux::meta::RuntimeObject &object) noexcept
{
    return captureLiteral(object);
}

FlowSourceResult<lux::meta::RuntimeObject> materializeFlowLiteral(const FlowSourceLiteral &literal,
                                                                  const lux::meta::RefType &type) noexcept
{
    if (literal.kind == EFlowLiteralKind::BOOLEAN && literal.value != "true" && literal.value != "false")
    {
        return fail(EFlowSourceError::INVALID_VALUE, "boolean");
    }
    if ((literal.kind == EFlowLiteralKind::NONE || literal.kind == EFlowLiteralKind::ZERO) && !literal.value.empty())
    {
        return fail(EFlowSourceError::INVALID_VALUE, "literal");
    }
    return materializeLiteral(literal, type);
}

FlowSourceResult<FlowSourceNode> captureFlowNode(const Node &node) noexcept
{
    FlowSourceNode item;
    item.id = node.id();
    item.operation = node.operation();
    item.name = node.name();
    item.creator = node.creatorName();
    if (binary(node.operation()))
    {
        item.parameters = FlowSourceType{std::string(static_cast<const BinaryOpNode &>(node).operandType()->name)};
    }
    else if (unary(node.operation()))
    {
        item.parameters = FlowSourceType{std::string(static_cast<const UnaryOpNode &>(node).operandType()->name)};
    }
    switch (node.operation())
    {
    case ENodeOperation::FUNC_DEF_START:
        item.parameters = FlowSourceSignature{captureArguments(static_cast<const FuncDefNode &>(node).argInfos()),
                                              captureArguments(static_cast<const FuncDefNode &>(node).retInfos())};
        break;
    case ENodeOperation::ON_EVENT:
        item.parameters =
            FlowSourceSignature{captureArguments(static_cast<const OnEventNode &>(node).paramInfos()), {}};
        break;
    case ENodeOperation::FUNC_RETURN:
        if (!static_cast<const FuncReturnNode &>(node).def())
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "function", node.id());
        }
        item.parameters = FlowSourceReference{static_cast<const FuncReturnNode &>(node).def()->id().value};
        break;
    case ENodeOperation::GRAPH_FUNC_CALL:
        if (!static_cast<const GraphFuncCallNode &>(node).callee())
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "function", node.id());
        }
        item.parameters = FlowSourceReference{static_cast<const GraphFuncCallNode &>(node).callee()->id().value};
        break;
    case ENodeOperation::GET_VARIABLE:
        item.parameters = FlowSourceReference{static_cast<const GetVariableNode &>(node).variableId()};
        break;
    case ENodeOperation::SET_VARIABLE:
        item.parameters = FlowSourceReference{static_cast<const SetVariableNode &>(node).variableId()};
        break;
    case ENodeOperation::GET_OBJECT:
        item.parameters =
            FlowSourceType{std::string(static_cast<const GetObjectNode &>(node).dataOutPin().info().type->name)};
        break;
    case ENodeOperation::SET_OBJECT:
        item.parameters =
            FlowSourceType{std::string(static_cast<const SetObjectNode &>(node).dataInPin().info().type->name)};
        break;
    case ENodeOperation::GET_FIELD: {
        const auto &field = static_cast<const GetFieldNode &>(node);
        item.parameters = FlowSourceField{std::string(field.ownerClass()->full_name), std::string(field.field()->name),
                                          std::string(field.field()->type.name)};
        break;
    }
    case ENodeOperation::SET_FIELD: {
        const auto &field = static_cast<const SetFieldNode &>(node);
        item.parameters = FlowSourceField{std::string(field.ownerClass()->full_name), std::string(field.field()->name),
                                          std::string(field.field()->type.name)};
        break;
    }
    case ENodeOperation::NATIVE_FUNC_CALL: {
        const auto &call = static_cast<const NativeFuncCall &>(node);
        FlowSourceNativeCall value;
        value.member = call.info().full_name;
        value.signature = call.info().type_signature;
        if (call.ownerType())
        {
            value.owner = call.ownerType()->name;
        }
        for (const auto &parameter : call.info().parameters)
        {
            value.parameters.arguments.push_back({std::string(parameter.name), std::string(parameter.type.name)});
        }
        value.parameters.results.push_back({{}, std::string(call.info().return_type.name)});
        item.parameters = std::move(value);
        break;
    }
    case ENodeOperation::SCRIPT_ABILITY_CALL: {
        const auto &ability = static_cast<const ScriptAbilityNode &>(node);
        item.parameters =
            FlowSourceAbility{std::string(ability.contract().name()), std::string(ability.method().name()),
                              ability.expectedSchemaVersion(), ability.expectedSchemaHash()};
        break;
    }
    case ENodeOperation::SCRIPT_EVENT_WAIT:
        item.parameters = static_cast<const ScriptEventAwaitNode &>(node).source();
        break;
    default:
        break;
    }
    for (const bool input : {true, false})
    {
        for (const auto *pin : input ? node.inPins() : node.outPins())
        {
            if (!pin)
            {
                return fail(EFlowSourceError::INVALID_VALUE, "pin", item.id);
            }
            FlowSourcePin value{pin->id(), pin->kind(), pin->name()};
            if (const auto *type = pinType(*pin))
            {
                value.type = type->name;
            }
            if (pin->kind() == EPinKind::DATA_IN)
            {
                auto literal = captureLiteral(static_cast<const DataInPin *>(pin)->constantData());
                if (!literal)
                {
                    auto error = literal.error();
                    error.node = item.id;
                    error.pin = pin->id();
                    return lux::cxx::unexpected(std::move(error));
                }
                value.literal = std::move(*literal);
            }
            (input ? item.inputs : item.outputs).push_back(std::move(value));
        }
    }
    return item;
}

FlowSourceResult<FlowSourceDocument> captureFlowSource(lux::asset::AssetId id, std::string name, const FlowGraph &graph,
                                                       FlowSourceLimits limits) noexcept
{
    FlowSourceDocument source{id, std::move(name)};
    for (const auto &storage : graph.nodes())
    {
        if (!storage.node)
        {
            return fail(EFlowSourceError::INVALID_VALUE, "node");
        }
        auto item = captureFlowNode(*storage.node);
        if (!item)
        {
            return lux::cxx::unexpected(item.error());
        }
        if (const auto *layout = graph.layout().find(storage.node->id()))
        {
            item->layout = *layout;
        }
        source.nodes.push_back(std::move(*item));
    }
    for (const auto &variable : graph.variables())
    {
        auto value = captureFlowVariable(variable);
        if (!value)
        {
            return lux::cxx::unexpected(value.error());
        }
        source.variables.push_back(std::move(*value));
    }
    for (const auto &link : graph.topology().links())
    {
        source.links.push_back({link.from, link.to});
    }
    for (const auto &item : graph.exports())
    {
        source.exports.push_back({item.id.value, item.symbol, item.entry_node_id, item.binding_hints});
    }
    std::ranges::sort(source.nodes, {}, [](const auto &value) { return value.id.value; });
    if (auto valid = validateFlowSource(source, limits); !valid)
    {
        return lux::cxx::unexpected(valid.error());
    }
    return source;
}

FlowSourceResult<void> validateFlowSourceConnection(const FlowSourcePin &from, const FlowSourcePin &to,
                                                    const FlowSourceEnvironment &environment) noexcept
{
    if (from.kind == EPinKind::EXEC_OUT && to.kind == EPinKind::EXEC_IN)
    {
        return {};
    }
    if (from.kind != EPinKind::DATA_OUT || to.kind != EPinKind::DATA_IN)
    {
        return fail(EFlowSourceError::INVALID_TOPOLOGY, "pin direction", {}, to.id);
    }
    const auto *output = findType(from.type, environment);
    const auto *input = findType(to.type, environment);
    if (!output)
    {
        return fail(EFlowSourceError::UNKNOWN_TYPE, from.type, {}, from.id);
    }
    if (!input)
    {
        return fail(EFlowSourceError::UNKNOWN_TYPE, to.type, {}, to.id);
    }
    if (!lux::meta::canInitialize(input, output))
    {
        return fail(EFlowSourceError::INVALID_TOPOLOGY, "pin conversion", {}, to.id);
    }
    return {};
}
FlowSourceResult<void> validateFlowSourceLiteral(const FlowSourcePin &pin,
                                                 const FlowSourceEnvironment &environment) noexcept
{
    if (pin.kind != EPinKind::DATA_IN)
    {
        return fail(EFlowSourceError::INVALID_VALUE, "literal direction", {}, pin.id);
    }
    const auto *type = findType(pin.type, environment);
    if (!type)
    {
        return fail(EFlowSourceError::UNKNOWN_TYPE, pin.type, {}, pin.id);
    }
    auto value = materializeLiteral(pin.literal, *type);
    if (!value)
    {
        auto failure = std::move(value.error());
        failure.pin = pin.id;
        return lux::cxx::unexpected(std::move(failure));
    }
    return {};
}
FlowSourceResult<FlowSourceVariable> captureFlowVariable(const FlowGraph::GraphVariable &variable) noexcept
{
    if (!variable.type)
    {
        return fail(EFlowSourceError::UNKNOWN_TYPE, "variable");
    }
    auto literal = captureLiteral(variable.default_value);
    if (!literal)
    {
        return lux::cxx::unexpected(literal.error());
    }
    return FlowSourceVariable{variable.id, variable.name, std::string(variable.type->name), std::move(*literal)};
}

FlowSourceResult<FlowGraph::GraphVariable> materializeFlowVariable(const FlowSourceVariable &variable,
                                                                   const FlowSourceEnvironment &environment) noexcept
{
    if (auto valid = validateFlowVariable(variable); !valid)
    {
        return lux::cxx::unexpected(valid.error());
    }
    const auto *type = findType(variable.type, environment);
    if (!type)
    {
        return fail(EFlowSourceError::UNKNOWN_TYPE, variable.type);
    }
    auto literal = materializeLiteral(variable.value, *type);
    if (!literal)
    {
        return lux::cxx::unexpected(literal.error());
    }
    return FlowGraph::GraphVariable{variable.id, variable.name, type, std::move(*literal)};
}

FlowSourceResult<FlowGraph> materializeFlowSource(const FlowSourceDocument &source,
                                                  const FlowSourceEnvironment &environment,
                                                  FlowSourceLimits limits) noexcept
{
    if (auto valid = validateFlowSource(source, limits); !valid)
    {
        return lux::cxx::unexpected(valid.error());
    }
    if (auto valid = validateFlowSourceEnvironment(environment, limits); !valid)
    {
        return lux::cxx::unexpected(valid.error());
    }
    FlowGraph graph;
    for (const auto &variable : source.variables)
    {
        auto value = materializeFlowVariable(variable, environment);
        if (!value)
        {
            return lux::cxx::unexpected(value.error());
        }
        if (!graph.addVariableWithId(value->id, std::move(value->name), value->type, std::move(value->default_value)))
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "variable");
        }
    }
    // Definitions first, then their callers/returns; source order is independent of construction order.
    for (const bool definitions : {true, false})
    {
        for (const auto &item : source.nodes)
        {
            if ((item.operation == ENodeOperation::FUNC_DEF_START) != definitions)
            {
                continue;
            }
            auto result = makeNode(item, graph, environment);
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
            auto node = std::move(*result);
            node->setName(item.name);
            node->setCreatorName(item.creator);
            if (node->inPins().size() != item.inputs.size() || node->outPins().size() != item.outputs.size())
            {
                return fail(EFlowSourceError::SCHEMA_MISMATCH, "pin count", item.id);
            }
            for (const bool input : {true, false})
            {
                const auto &pins = input ? node->inPins() : node->outPins();
                const auto &saved = input ? item.inputs : item.outputs;
                for (std::size_t i{}; i < pins.size(); ++i)
                {
                    auto &pin = *pins[i];
                    const auto *type = pinType(pin);
                    const bool mismatch =
                        pin.kind() != saved[i].kind || (type ? type->name != saved[i].type : !saved[i].type.empty());
                    if (mismatch)
                    {
                        return fail(EFlowSourceError::SCHEMA_MISMATCH, "pin type", item.id, saved[i].id);
                    }
                    pin.setName(saved[i].name);
                    if (!FlowGraph::assignDetachedPinId(pin, saved[i].id))
                    {
                        return fail(EFlowSourceError::INVALID_IDENTITY, "pin", item.id, saved[i].id);
                    }
                    if (pin.kind() == EPinKind::DATA_IN)
                    {
                        auto literal = materializeLiteral(saved[i].literal, *type);
                        if (!literal)
                        {
                            return lux::cxx::unexpected(literal.error());
                        }
                        auto &target = static_cast<DataInPin &>(pin);
                        // This is reconstruction of an exact declared type, not an implicit assignment.
                        // In particular, the legacy conversion table does not accept even void* -> void*.
                        target.constantData() = std::move(*literal);
                    }
                }
            }
            if (graph.addNodesWithId(item.id, std::move(node)) == (std::numeric_limits<std::size_t>::max)())
            {
                return fail(EFlowSourceError::INVALID_TOPOLOGY, "node", item.id);
            }
            if (!graph.layout().set(item.id, item.layout))
            {
                return fail(EFlowSourceError::INVALID_VALUE, "layout", item.id);
            }
        }
    }
    for (const auto &link : source.links)
    {
        auto *from = graph.findPin(link.from);
        auto *to = graph.findPin(link.to);
        if (!from || !to || graph.connect(*from, *to) != ELinkError::SUCCESS)
        {
            return fail(EFlowSourceError::INVALID_TOPOLOGY, "link", {}, link.to);
        }
    }
    for (const auto &item : source.exports)
    {
        if (!graph.addExport({FlowForgeExportNodeId{item.id}, item.entry, item.symbol, item.hints}))
        {
            return fail(EFlowSourceError::INVALID_VALUE, "export", item.entry);
        }
    }
    return graph;
}
} // namespace lux::flowforge
