#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/detail/FlowNodeIdentity.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/ObjectNodes.hpp>
#include <lux/engine/flowforge/ScalarNodes.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityPayload.hpp>
#include <lux/engine/flowforge/script/ScriptEventPayload.hpp>
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

        template <class Fn> bool visitScalar(const lux::meta::RefType& type, Fn&& fn)
        {
            if (type.qtype.qual != static_cast<unsigned>(lux::meta::ETypeQual::VALUE))
            {
                return false;
            }
            using B = lux::meta::EBaseType;
            switch (static_cast<B>(type.qtype.base))
            {
            case B::BOOL:
                return type.size == sizeof(bool) && fn.template operator()<bool>();
            case B::INT8:
                return type.size == 1 && fn.template operator()<std::int8_t>();
            case B::UINT8:
                return type.size == 1 && fn.template operator()<std::uint8_t>();
            case B::INT16:
                return type.size == 2 && fn.template operator()<std::int16_t>();
            case B::UINT16:
                return type.size == 2 && fn.template operator()<std::uint16_t>();
            case B::INT32:
                return type.size == 4 && fn.template operator()<std::int32_t>();
            case B::UINT32:
                return type.size == 4 && fn.template operator()<std::uint32_t>();
            case B::INT64:
                return type.size == 8 && fn.template operator()<std::int64_t>();
            case B::UINT64:
                return type.size == 8 && fn.template operator()<std::uint64_t>();
            case B::FLOAT:
                return type.size == 4 && fn.template operator()<float>();
            case B::DOUBLE:
                return type.size == 8 && fn.template operator()<double>();
            default:
                return false;
            }
        }

        FlowSourceResult<FlowSourceLiteral> captureLiteral(const lux::meta::RuntimeObject& object) noexcept
        {
            if (!object.isValid())
            {
                return FlowSourceLiteral{};
            }
            FlowSourceLiteral literal;
            const bool scalar = visitScalar(
                *object.type(),
                [&]<class T>()
                {
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
                }
            );
            if (scalar)
            {
                return literal;
            }
            // Null pointer/zero trivial object only. Never serialize a process address or opaque object bytes.
            const auto* bytes = static_cast<const std::byte*>(object.data());
            if (std::all_of(bytes, bytes + object.type()->size, [](std::byte value) { return value == std::byte{}; }))
            {
                return FlowSourceLiteral{EFlowLiteralKind::ZERO, {}};
            }
            return fail(EFlowSourceError::UNSUPPORTED_LITERAL, std::string(object.type()->name));
        }

        FlowSourceResult<lux::meta::RuntimeObject> materializeLiteral(
            const FlowSourceLiteral& literal,
            const lux::meta::RefType& type
        ) noexcept
        {
            if (literal.kind == EFlowLiteralKind::NONE)
            {
                return lux::meta::RuntimeObject{};
            }
            auto result = lux::meta::RuntimeObject::defaultOf(type);
            if (!result)
            {
                return fail(EFlowSourceError::UNSUPPORTED_LITERAL, std::string(type.name));
            }
            if (literal.kind == EFlowLiteralKind::ZERO)
            {
                return std::move(*result);
            }
            const bool valid = visitScalar(
                type,
                [&]<class T>()
                {
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
                    std::memcpy(result->data(), &value, sizeof(T));
                    return true;
                }
            );
            if (!valid)
            {
                return fail(EFlowSourceError::UNSUPPORTED_LITERAL, std::string(type.name));
            }
            return std::move(*result);
        }

        EFlowSourcePinKind sourcePinKind(const FlowPinDeclaration& pin) noexcept
        {
            const bool input = pin.direction == graph::EPinDirection::INPUT;
            if (pin.role == EFlowPinRole::EXECUTION)
            {
                return input ? EFlowSourcePinKind::EXEC_IN : EFlowSourcePinKind::EXEC_OUT;
            }
            return input ? EFlowSourcePinKind::DATA_IN : EFlowSourcePinKind::DATA_OUT;
        }

        std::vector<FlowSourceArgument> captureArguments(const std::vector<FuncArgInfo>& values)
        {
            std::vector<FlowSourceArgument> result;
            for (const auto& value : values)
            {
                result.push_back({value.name, value.type ? std::string(value.type->name) : std::string{}});
            }
            return result;
        }

        const lux::meta::RefType* findType(std::string_view name, const FlowSourceEnvironment& environment) noexcept
        {
            const lux::meta::RefType* result{};
            const auto builtin = [&]<class... T>()
            {
                ((name == lux::meta::builtin_ref_type_ptr<T>()->name ? result = lux::meta::builtin_ref_type_ptr<T>()
                                                                     : result),
                 ...);
            };
            builtin.template operator(
            )<void,
              bool,
              char,
              signed char,
              unsigned char,
              short,
              unsigned short,
              int,
              unsigned int,
              long,
              unsigned long,
              long long,
              unsigned long long,
              float,
              double,
              void*>();
            if (result)
            {
                return result;
            }
            for (const auto* type : environment.types)
            {
                if (type && type->name == name)
                {
                    return type;
                }
            }
            for (const auto* type : environment.classes)
            {
                if (type && type->type.name == name)
                {
                    return &type->type;
                }
            }
            return nullptr;
        }

        const lux::meta::RefClass* findClass(std::string_view name, const FlowSourceEnvironment& environment) noexcept
        {
            for (const auto* type : environment.classes)
            {
                if (type && type->full_name == name)
                {
                    return type;
                }
            }
            return nullptr;
        }

        FlowSourceResult<std::vector<FuncArgInfo>> materializeArguments(
            const std::vector<FlowSourceArgument>& values,
            const FlowSourceEnvironment& environment
        ) noexcept
        {
            std::vector<FuncArgInfo> result;
            for (const auto& value : values)
            {
                const auto* type = findType(value.type, environment);
                if (!type)
                {
                    return fail(EFlowSourceError::UNKNOWN_TYPE, value.type);
                }
                result.push_back({type, value.name});
            }
            return result;
        }

        bool signatureMatches(const lux::meta::RefInvokable& info, const FlowSourceNativeCall& source) noexcept
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
            return source.parameters.results.size() == 1 &&
                   info.return_type.name == source.parameters.results.front().type;
        }

        FlowSourceResult<FlowNode> makeNode(
            const FlowSourceNode& source,
            const FlowGraph& graph,
            const std::map<NodeId, FlowNode>& prepared,
            const FlowNodeCatalog& builtins,
            const FlowSourceEnvironment& environment
        ) noexcept
        {
            const auto operation = detail::builtinSourceKind(source.type);
            if (operation == detail::EBuiltinSourceKind::EXTENSION)
            {
                const auto definition =
                    environment.nodes ? environment.nodes->find(graph::nodeTypeId(source.type)) : nullptr;
                if (!definition)
                {
                    return fail(EFlowSourceError::UNKNOWN_NODE_KIND, source.type, source.id);
                }
                const bool is_identity_mismatch = definition->identity().canonical_name != source.type ||
                                                  definition->identity().version != source.version;
                if (is_identity_mismatch)
                {
                    return fail(EFlowSourceError::SCHEMA_MISMATCH, source.type, source.id);
                }
                auto payload = definition->decode(std::get<FlowSourcePayload>(source.parameters).bytes);
                if (!payload)
                {
                    FlowSourceFailure error{EFlowSourceError::NODE_CODEC_FAILURE, source.type, source.id};
                    error.cause = std::move(payload.error());
                    return cxx::unexpected(std::move(error));
                }
                auto node = createFlowNode(definition, std::move(*payload));
                if (!node)
                {
                    FlowSourceFailure error{EFlowSourceError::NODE_CODEC_FAILURE, source.type, source.id};
                    error.cause = std::move(node.error());
                    return cxx::unexpected(std::move(error));
                }
                return std::move(*node);
            }
            if (detail::isScalarSourceKind(operation))
            {
                const auto* type = findType(std::get<FlowSourceType>(source.parameters).name, environment);
                if (!type)
                {
                    return fail(
                        EFlowSourceError::UNKNOWN_TYPE,
                        std::get<FlowSourceType>(source.parameters).name,
                        source.id
                    );
                }
                auto definition = builtins.find(graph::nodeTypeId(source.type));
                auto payload = definition->create();
                if (!payload)
                {
                    FlowSourceFailure error{EFlowSourceError::NODE_CODEC_FAILURE, source.type, source.id};
                    error.cause = std::move(payload.error());
                    return cxx::unexpected(std::move(error));
                }
                payload->get<ScalarNodePayload>()->operand_type = type;
                auto node = createFlowNode(std::move(definition), std::move(*payload));
                if (!node)
                {
                    FlowSourceFailure error{EFlowSourceError::NODE_CODEC_FAILURE, source.type, source.id};
                    error.cause = std::move(node.error());
                    return cxx::unexpected(std::move(error));
                }
                return std::move(*node);
            }
            // The fixed wire adapters only resolve saved metadata. The registered definition
            // remains the sole schema, validation and compile provider.
            const auto make = [&]<class T>(T value) noexcept -> FlowSourceResult<FlowNode>
            {
                auto definition = builtins.find(graph::nodeTypeId(source.type));
                if (!definition)
                {
                    return fail(EFlowSourceError::UNKNOWN_NODE_KIND, source.type, source.id);
                }
                auto payload = definition->create();
                if (!payload)
                {
                    FlowSourceFailure error{EFlowSourceError::NODE_CODEC_FAILURE, source.type, source.id};
                    error.cause = std::move(payload.error());
                    return cxx::unexpected(std::move(error));
                }
                auto* target = payload->get<T>();
                if (!target)
                {
                    return fail(EFlowSourceError::SCHEMA_MISMATCH, source.type, source.id);
                }
                *target = std::move(value);
                auto node = createFlowNode(std::move(definition), std::move(*payload));
                if (!node)
                {
                    FlowSourceFailure error{EFlowSourceError::NODE_CODEC_FAILURE, source.type, source.id};
                    error.cause = std::move(node.error());
                    return cxx::unexpected(std::move(error));
                }
                return std::move(*node);
            };
            switch (operation)
            {
            case detail::EBuiltinSourceKind::START:
                return make(StartPayload{});
            case detail::EBuiltinSourceKind::BRANCH:
                return make(BranchPayload{});
            case detail::EBuiltinSourceKind::FOR_LOOP:
                return make(ForLoopPayload{});
            case detail::EBuiltinSourceKind::WHILE_LOOP:
                return make(WhileLoopPayload{});
            case detail::EBuiltinSourceKind::RETURN:
                return make(ReturnPayload{});
            case detail::EBuiltinSourceKind::BREAK:
                return make(BreakPayload{});
            case detail::EBuiltinSourceKind::SEQUENCE:
            {
                if (source.outputs.empty())
                {
                    return fail(EFlowSourceError::SCHEMA_MISMATCH, "sequence outputs", source.id);
                }
                return make(SequencePayload{source.outputs.size() - 1});
            }
            case detail::EBuiltinSourceKind::FUNC_DEF_START:
            case detail::EBuiltinSourceKind::ON_EVENT:
            {
                auto args =
                    materializeArguments(std::get<FlowSourceSignature>(source.parameters).arguments, environment);
                if (!args)
                {
                    return lux::cxx::unexpected(args.error());
                }
                if (operation == detail::EBuiltinSourceKind::ON_EVENT)
                {
                    return make(EventEntryPayload{std::move(*args)});
                }
                auto results =
                    materializeArguments(std::get<FlowSourceSignature>(source.parameters).results, environment);
                if (!results)
                {
                    return lux::cxx::unexpected(results.error());
                }
                return make(FunctionPayload{std::move(*args), std::move(*results)});
            }
            case detail::EBuiltinSourceKind::FUNC_RETURN:
            case detail::EBuiltinSourceKind::GRAPH_FUNC_CALL:
            {
                const NodeId reference{std::get<FlowSourceReference>(source.parameters).id};
                const auto found = prepared.find(reference);
                const auto* target = found == prepared.end() ? nullptr : found->second.payload.get<FunctionPayload>();
                if (!target)
                {
                    return fail(EFlowSourceError::INVALID_IDENTITY, "function", source.id);
                }
                const auto& definition = *target;
                if (operation == detail::EBuiltinSourceKind::FUNC_RETURN)
                {
                    return make(FunctionReturnPayload{reference, definition.results});
                }
                return make(FunctionCallPayload{reference, definition.arguments, definition.results});
            }
            case detail::EBuiltinSourceKind::GET_VARIABLE:
            case detail::EBuiltinSourceKind::SET_VARIABLE:
            {
                const auto* variable = graph.findVariable(std::get<FlowSourceReference>(source.parameters).id);
                if (!variable)
                {
                    return fail(EFlowSourceError::INVALID_IDENTITY, "variable", source.id);
                }
                if (operation == detail::EBuiltinSourceKind::GET_VARIABLE)
                {
                    return make(GetVariablePayload{variable->id, variable->type});
                }
                return make(SetVariablePayload{variable->id, variable->type});
            }
            case detail::EBuiltinSourceKind::GET_OBJECT:
            case detail::EBuiltinSourceKind::SET_OBJECT:
            {
                const auto* type = findType(std::get<FlowSourceType>(source.parameters).name, environment);
                if (!type)
                {
                    return fail(
                        EFlowSourceError::UNKNOWN_TYPE,
                        std::get<FlowSourceType>(source.parameters).name,
                        source.id
                    );
                }
                if (operation == detail::EBuiltinSourceKind::GET_OBJECT)
                {
                    return make(GetObjectPayload{type});
                }
                return make(SetObjectPayload{type});
            }
            case detail::EBuiltinSourceKind::GET_FIELD:
            case detail::EBuiltinSourceKind::SET_FIELD:
            {
                const auto& field_source = std::get<FlowSourceField>(source.parameters);
                const auto* owner = findClass(field_source.owner, environment);
                if (owner)
                {
                    for (const auto& field : owner->fields)
                    {
                        const bool is_matching_field =
                            field.name == field_source.member && field.type.name == field_source.type;
                        if (is_matching_field)
                        {
                            if (operation == detail::EBuiltinSourceKind::GET_FIELD)
                            {
                                return make(GetFieldPayload{owner, &field});
                            }
                            return make(SetFieldPayload{owner, &field});
                        }
                    }
                }
                return fail(
                    EFlowSourceError::UNKNOWN_REFLECTION_MEMBER,
                    field_source.owner + "::" + field_source.member,
                    source.id
                );
            }
            case detail::EBuiltinSourceKind::NATIVE_FUNC_CALL:
            {
                const auto& call = std::get<FlowSourceNativeCall>(source.parameters);
                const auto create_call = [&](const meta::RefInvokable& signature,
                                             const meta::RefType* receiver) noexcept -> FlowSourceResult<FlowNode>
                {
                    auto code = environment.code_lifetime ? object::CodeLease::plugin(environment.code_lifetime)
                                                          : object::CodeLease::builtin();
                    auto definition = NativeCallDefinition::create(signature, std::move(code), receiver);
                    if (!definition)
                    {
                        return fail(EFlowSourceError::SCHEMA_MISMATCH, call.member, source.id);
                    }
                    return make(NativeCallPayload{std::move(*definition)});
                };
                if (call.owner.empty())
                {
                    for (const auto* function : environment.functions)
                    {
                        if (function && signatureMatches(function->invokable, call))
                        {
                            return create_call(function->invokable, nullptr);
                        }
                    }
                }
                else if (const auto* owner = findClass(call.owner, environment))
                {
                    for (const auto& method : owner->methods)
                    {
                        if (signatureMatches(method.invokable, call))
                        {
                            return create_call(method.invokable, &owner->type);
                        }
                    }
                }
                return fail(EFlowSourceError::UNKNOWN_REFLECTION_MEMBER, call.member, source.id);
            }
            case detail::EBuiltinSourceKind::SCRIPT_ABILITY_CALL:
            {
                const auto& ability = std::get<FlowSourceAbility>(source.parameters);
                const auto* description = environment.abilities.find(
                    lux::script::ScriptApiContractIdView{ability.contract},
                    lux::script::ScriptApiMethodIdView{ability.method}
                );
                if (!description)
                {
                    return fail(EFlowSourceError::UNKNOWN_ABILITY, ability.method, source.id);
                }
                const bool is_schema_mismatch = description->schema_version != ability.schema_version ||
                                                description->schema_hash != ability.schema_hash;
                if (is_schema_mismatch)
                {
                    return fail(EFlowSourceError::SCHEMA_MISMATCH, ability.method, source.id);
                }
                return make(ScriptAbilityPayload{*description});
            }
            case detail::EBuiltinSourceKind::SCRIPT_EVENT_WAIT:
            {
                const auto& saved_event = std::get<lux::script::ScriptEventSourceDescription>(source.parameters);
                for (const auto& event : environment.events)
                {
                    const bool is_matching_event =
                        event.system_id == saved_event.system_id && event.event_id == saved_event.event_id;
                    if (is_matching_event)
                    {
                        if (event != saved_event)
                        {
                            return fail(EFlowSourceError::SCHEMA_MISMATCH, "event", source.id);
                        }
                        return make(ScriptEventPayload{event});
                    }
                }
                return fail(EFlowSourceError::SCHEMA_MISMATCH, "missing event", source.id);
            }
            default:
                return fail(EFlowSourceError::UNKNOWN_NODE_KIND, "operation", source.id);
            }
        }
    } // namespace

    FlowSourceResult<void> validateFlowSourceEnvironment(
        const FlowSourceEnvironment& environment,
        FlowSourceLimits limits
    ) noexcept
    {
        const bool too_many =
            environment.types.size() > limits.max_pins || environment.classes.size() > limits.max_nodes ||
            environment.functions.size() > limits.max_nodes ||
            environment.abilities.nodes().size() > limits.max_nodes || environment.events.size() > limits.max_nodes;
        if (too_many)
        {
            return fail(EFlowSourceError::LIMIT_EXCEEDED, "metadata");
        }
        std::map<std::string_view, const lux::meta::RefType*> types;
        for (const auto* type : environment.types)
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
        for (const auto* type : environment.classes)
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
        for (const auto* function : environment.functions)
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
        if (auto valid = validateScriptAbilityNodes(environment.abilities.nodes()); !valid)
        {
            return fail(EFlowSourceError::SCHEMA_MISMATCH, "metadata.ability");
        }
        std::set<std::pair<std::uint64_t, std::uint64_t>> events;
        for (const auto& event : environment.events)
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

    FlowSourceResult<std::vector<FuncArgInfo>> materializeFlowArguments(
        std::span<const FlowSourceArgument> values,
        const FlowSourceEnvironment& environment,
        FlowSourceLimits limits
    ) noexcept
    {
        if (values.size() > limits.max_pins)
        {
            return fail(EFlowSourceError::LIMIT_EXCEEDED, "signature");
        }
        std::vector<FuncArgInfo> result;
        result.reserve(values.size());
        for (const auto& value : values)
        {
            const bool invalid_name = value.name.empty() || value.name.size() > limits.max_string_bytes ||
                                      value.name.find('\0') != std::string::npos;
            const bool duplicate_name = std::ranges::find(result, value.name, &FuncArgInfo::name) != result.end();
            if (invalid_name || duplicate_name)
            {
                return fail(EFlowSourceError::INVALID_VALUE, "argument.name");
            }
            const auto* type = findType(value.type, environment);
            if (!type || !type->size)
            {
                return fail(EFlowSourceError::UNKNOWN_TYPE, value.type);
            }
            result.push_back({type, value.name});
        }
        return result;
    }

    FlowSourceResult<FlowSourceLiteral> captureFlowLiteral(const lux::meta::RuntimeObject& object) noexcept
    {
        return captureLiteral(object);
    }

    FlowSourceResult<lux::meta::RuntimeObject> materializeFlowLiteral(
        const FlowSourceLiteral& literal,
        const lux::meta::RefType& type
    ) noexcept
    {
        if (literal.kind == EFlowLiteralKind::BOOLEAN && literal.value != "true" && literal.value != "false")
        {
            return fail(EFlowSourceError::INVALID_VALUE, "boolean");
        }
        if ((literal.kind == EFlowLiteralKind::NONE || literal.kind == EFlowLiteralKind::ZERO) &&
            !literal.value.empty())
        {
            return fail(EFlowSourceError::INVALID_VALUE, "literal");
        }
        return materializeLiteral(literal, type);
    }

    FlowSourceResult<FlowSourceNode> captureFlowNode(const FlowGraph& graph, NodeId id) noexcept
    {
        const auto* found = graph.node(id);
        if (!found)
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "node", id);
        }
        const auto& node = *found;
        FlowSourceNode item;
        item.id = id;
        const auto& definition = *node.definition;
        const auto find_node = [&](NodeId target) noexcept { return graph.node(target); };
        const auto find_variable_type = [&](std::uint64_t variable) noexcept -> const meta::RefType*
        {
            const auto* value = graph.findVariable(variable);
            return value ? value->type : nullptr;
        };
        auto references = definition.validateReferences(node.payload, {find_node, find_variable_type});
        if (!references)
        {
            FlowSourceFailure error{EFlowSourceError::INVALID_IDENTITY, "references", id};
            error.cause = std::move(references.error());
            return cxx::unexpected(std::move(error));
        }
        item.type = definition.identity().canonical_name;
        item.version = definition.identity().version;
        item.name = node.name;
        item.creator = node.creator;
        const auto operation = detail::builtinSourceKind(item.type);
        const bool is_scalar = detail::isScalarSourceKind(operation);
        const bool is_extension = operation == detail::EBuiltinSourceKind::EXTENSION;
        if (is_scalar || is_extension)
        {
            auto encoded = definition.encode(node.payload);
            if (!encoded)
            {
                FlowSourceFailure error{EFlowSourceError::NODE_CODEC_FAILURE, item.type, id};
                error.cause = std::move(encoded.error());
                return cxx::unexpected(std::move(error));
            }
            if (is_scalar)
            {
                item.parameters = FlowSourceType{std::move(*encoded)};
            }
            else
            {
                item.parameters = FlowSourcePayload{std::move(*encoded)};
            }
        }
        switch (operation)
        {
        case detail::EBuiltinSourceKind::FUNC_DEF_START:
            item.parameters = FlowSourceSignature{
                captureArguments(node.payload.get<FunctionPayload>()->arguments),
                captureArguments(node.payload.get<FunctionPayload>()->results)
            };
            break;
        case detail::EBuiltinSourceKind::ON_EVENT:
            item.parameters =
                FlowSourceSignature{captureArguments(node.payload.get<EventEntryPayload>()->parameters), {}};
            break;
        case detail::EBuiltinSourceKind::FUNC_RETURN:
            item.parameters = FlowSourceReference{node.payload.get<FunctionReturnPayload>()->definition.value};
            break;
        case detail::EBuiltinSourceKind::GRAPH_FUNC_CALL:
            item.parameters = FlowSourceReference{node.payload.get<FunctionCallPayload>()->callee.value};
            break;
        case detail::EBuiltinSourceKind::GET_VARIABLE:
            item.parameters = FlowSourceReference{node.payload.get<GetVariablePayload>()->variable};
            break;
        case detail::EBuiltinSourceKind::SET_VARIABLE:
            item.parameters = FlowSourceReference{node.payload.get<SetVariablePayload>()->variable};
            break;
        case detail::EBuiltinSourceKind::GET_OBJECT:
            item.parameters = FlowSourceType{std::string(node.payload.get<GetObjectPayload>()->type->name)};
            break;
        case detail::EBuiltinSourceKind::SET_OBJECT:
            item.parameters = FlowSourceType{std::string(node.payload.get<SetObjectPayload>()->type->name)};
            break;
        case detail::EBuiltinSourceKind::GET_FIELD:
        {
            const auto& field = *node.payload.get<GetFieldPayload>();
            item.parameters = FlowSourceField{
                std::string(field.owner->full_name),
                std::string(field.field->name),
                std::string(field.field->type.name)
            };
            break;
        }
        case detail::EBuiltinSourceKind::SET_FIELD:
        {
            const auto& field = *node.payload.get<SetFieldPayload>();
            item.parameters = FlowSourceField{
                std::string(field.owner->full_name),
                std::string(field.field->name),
                std::string(field.field->type.name)
            };
            break;
        }
        case detail::EBuiltinSourceKind::NATIVE_FUNC_CALL:
        {
            const auto& call = *node.payload.get<NativeCallPayload>()->definition;
            FlowSourceNativeCall value;
            value.member = call.signature().full_name;
            value.signature = call.signature().type_signature;
            if (call.receiver())
            {
                value.owner = call.receiver()->name;
            }
            for (const auto& parameter : call.signature().parameters)
            {
                value.parameters.arguments.push_back({std::string(parameter.name), std::string(parameter.type.name)});
            }
            value.parameters.results.push_back({{}, std::string(call.signature().return_type.name)});
            item.parameters = std::move(value);
            break;
        }
        case detail::EBuiltinSourceKind::SCRIPT_ABILITY_CALL:
        {
            const auto& ability = *node.payload.get<ScriptAbilityPayload>();
            item.parameters = FlowSourceAbility{
                std::string(ability.contract().name()),
                std::string(ability.method().name()),
                ability.expectedSchemaVersion(),
                ability.expectedSchemaHash()
            };
            break;
        }
        case detail::EBuiltinSourceKind::SCRIPT_EVENT_WAIT:
            item.parameters = node.payload.get<ScriptEventPayload>()->source();
            break;
        default:
            break;
        }
        auto declarations = definition.describePins(node.payload);
        if (!declarations)
        {
            FlowSourceFailure error{EFlowSourceError::SCHEMA_MISMATCH, "pins", id};
            error.cause = std::move(declarations.error());
            return cxx::unexpected(std::move(error));
        }
        for (const auto& declaration : *declarations)
        {
            const auto pin_id = graph.pinId(id, declaration.semantic);
            const auto* pin = graph.pin(pin_id);
            const auto* record = graph.topology().findPin(pin_id);
            if (!pin || !record)
            {
                return fail(EFlowSourceError::INVALID_TOPOLOGY, "pin", id, pin_id);
            }
            FlowSourcePin value{pin_id, sourcePinKind(declaration), pin->name};
            value.semantic = record->semantic;
            if (pin->type)
            {
                value.type = pin->type->name;
            }
            if (value.kind == EFlowSourcePinKind::DATA_IN)
            {
                auto literal = captureLiteral(pin->default_value);
                if (!literal)
                {
                    auto error = std::move(literal.error());
                    error.node = id;
                    error.pin = pin_id;
                    return cxx::unexpected(std::move(error));
                }
                value.literal = std::move(*literal);
            }
            const bool input = record->direction == graph::EPinDirection::INPUT;
            (input ? item.inputs : item.outputs).push_back(std::move(value));
        }
        return item;
    }

    FlowSourceResult<FlowSource> captureFlowSource(
        lux::asset::AssetId id,
        std::string name,
        const FlowGraph& graph,
        FlowSourceLimits limits
    ) noexcept
    {
        FlowSource source{id, std::move(name)};
        for (const auto& [node_id, node] : graph.nodes())
        {
            if (!node)
            {
                return fail(EFlowSourceError::INVALID_VALUE, "node");
            }
            auto item = captureFlowNode(graph, node_id);
            if (!item)
            {
                return lux::cxx::unexpected(item.error());
            }
            if (const auto* layout = graph.layout().find(node_id))
            {
                item->layout = *layout;
            }
            source.nodes.push_back(std::move(*item));
        }
        for (const auto& variable : graph.variables())
        {
            auto value = captureFlowVariable(variable);
            if (!value)
            {
                return lux::cxx::unexpected(value.error());
            }
            source.variables.push_back(std::move(*value));
        }
        for (const auto& link : graph.topology().links())
        {
            source.links.push_back({link.from, link.to});
        }
        for (const auto& item : graph.exports())
        {
            source.exports.push_back({item.id.value, item.symbol, item.entry_node_id, item.binding_hints});
        }
        std::ranges::sort(source.nodes, {}, [](const auto& value) { return value.id.value; });
        if (auto valid = validateFlowSource(source, limits); !valid)
        {
            return lux::cxx::unexpected(valid.error());
        }
        return source;
    }

    FlowSourceResult<void> validateFlowSourceConnection(
        const FlowSourcePin& from,
        const FlowSourcePin& to,
        const FlowSourceEnvironment& environment
    ) noexcept
    {
        if (from.kind == EFlowSourcePinKind::EXEC_OUT && to.kind == EFlowSourcePinKind::EXEC_IN)
        {
            return {};
        }
        if (from.kind != EFlowSourcePinKind::DATA_OUT || to.kind != EFlowSourcePinKind::DATA_IN)
        {
            return fail(EFlowSourceError::INVALID_TOPOLOGY, "pin direction", {}, to.id);
        }
        const auto* output = findType(from.type, environment);
        const auto* input = findType(to.type, environment);
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

    FlowSourceResult<void> validateFlowSourceLiteral(
        const FlowSourcePin& pin,
        const FlowSourceEnvironment& environment
    ) noexcept
    {
        if (pin.kind != EFlowSourcePinKind::DATA_IN)
        {
            return fail(EFlowSourceError::INVALID_VALUE, "literal direction", {}, pin.id);
        }
        const auto* type = findType(pin.type, environment);
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

    FlowSourceResult<FlowSourceVariable> captureFlowVariable(const FlowGraph::GraphVariable& variable) noexcept
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

    FlowSourceResult<FlowGraph::GraphVariable> materializeFlowVariable(
        const FlowSourceVariable& variable,
        const FlowSourceEnvironment& environment
    ) noexcept
    {
        if (auto valid = validateFlowVariable(variable); !valid)
        {
            return lux::cxx::unexpected(valid.error());
        }
        const auto* type = findType(variable.type, environment);
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

    FlowSourceResult<FlowGraph> materializeFlowSource(
        const FlowSource& source,
        const FlowSourceEnvironment& environment,
        FlowSourceLimits limits
    ) noexcept
    {
        if (auto valid = validateFlowSource(source, limits); !valid)
        {
            return lux::cxx::unexpected(valid.error());
        }
        if (auto valid = validateFlowSourceEnvironment(environment, limits); !valid)
        {
            return lux::cxx::unexpected(valid.error());
        }
        FlowNodeCatalog builtins;
        const auto builtin_code = environment.code_lifetime ? object::CodeLease::plugin(environment.code_lifetime)
                                                            : object::CodeLease::builtin();
        const auto special = std::array{
            nativeCallRegistration(builtin_code),
            scriptAbilityRegistration(builtin_code),
            scriptEventRegistration(builtin_code)
        };
        const bool registered = builtins.add(scalarNodeRegistrations(builtin_code)) &&
                                builtins.add(controlNodeRegistrations(builtin_code)) &&
                                builtins.add(functionNodeRegistrations(builtin_code)) &&
                                builtins.add(objectNodeRegistrations(builtin_code)) && builtins.add(special);
        if (!registered)
        {
            return fail(EFlowSourceError::SCHEMA_MISMATCH, "builtin definitions");
        }
        FlowGraph graph;
        for (const auto& variable : source.variables)
        {
            auto value = materializeFlowVariable(variable, environment);
            if (!value)
            {
                return lux::cxx::unexpected(value.error());
            }
            if (!graph.addVariableWithId(
                    value->id,
                    std::move(value->name),
                    value->type,
                    std::move(value->default_value)
                ))
            {
                return fail(EFlowSourceError::INVALID_IDENTITY, "variable");
            }
        }
        // Detached semantic values resolve forward function references without publishing a
        // partially constructed graph. One GraphEdit validates the complete candidate below.
        std::map<NodeId, FlowNode> prepared;
        std::vector<std::vector<FlowPinEntry>> restored;
        std::vector<FlowNodeEntry> entries;
        std::vector<graph::GraphLayoutEntry> placements;
        restored.reserve(source.nodes.size());
        entries.reserve(source.nodes.size());
        placements.reserve(source.nodes.size());
        for (const bool definitions : {true, false})
        {
            for (const auto& item : source.nodes)
            {
                if ((detail::builtinSourceKind(item.type) == detail::EBuiltinSourceKind::FUNC_DEF_START) != definitions)
                {
                    continue;
                }
                auto result = makeNode(item, graph, prepared, builtins, environment);
                if (!result)
                {
                    return cxx::unexpected(std::move(result.error()));
                }
                result->name = item.name;
                result->creator = item.creator;
                prepared.emplace(item.id, std::move(*result));
            }
        }
        for (const auto& item : source.nodes)
        {
            const auto& node = prepared.at(item.id);
            auto declarations = node.definition->describePins(node.payload);
            if (!declarations)
            {
                FlowSourceFailure error{EFlowSourceError::SCHEMA_MISMATCH, "pins", item.id};
                error.cause = std::move(declarations.error());
                return cxx::unexpected(std::move(error));
            }
            auto& pins = restored.emplace_back();
            pins.reserve(declarations->size());
            std::size_t input_index{}, output_index{};
            for (const auto& declaration : *declarations)
            {
                const bool input = declaration.direction == graph::EPinDirection::INPUT;
                const auto& saved = input ? item.inputs : item.outputs;
                auto& index = input ? input_index : output_index;
                if (index == saved.size())
                {
                    return fail(EFlowSourceError::SCHEMA_MISMATCH, "pin count", item.id);
                }
                const auto& value = saved[index++];
                const auto* type = declaration.type;
                const bool is_kind_mismatch = sourcePinKind(declaration) != value.kind;
                const bool is_semantic_mismatch = declaration.semantic != value.semantic;
                const bool is_type_mismatch = type ? type->name != value.type : !value.type.empty();
                if (is_kind_mismatch || is_semantic_mismatch || is_type_mismatch)
                {
                    return fail(EFlowSourceError::SCHEMA_MISMATCH, "pin type", item.id, value.id);
                }
                const bool single = declaration.role == EFlowPinRole::EXECUTION ? !input : input;
                FlowPinEntry entry{
                    {value.id,
                     item.id,
                     declaration.direction,
                     single ? std::uint8_t{1} : graph::kUnlimitedFan,
                     declaration.semantic},
                    {value.name, declaration.role, type, declaration.allow_default, declaration.necessary, {}}
                };
                if (value.kind == EFlowSourcePinKind::DATA_IN)
                {
                    auto literal = materializeLiteral(value.literal, *type);
                    if (!literal)
                    {
                        auto error = std::move(literal.error());
                        error.node = item.id;
                        error.pin = value.id;
                        return cxx::unexpected(std::move(error));
                    }
                    // Exact-type restoration is not an implicit assignment (notably void*).
                    entry.value.default_value = std::move(*literal);
                }
                pins.push_back(std::move(entry));
            }
            const bool is_count_mismatch = input_index != item.inputs.size() || output_index != item.outputs.size();
            if (is_count_mismatch)
            {
                return fail(EFlowSourceError::SCHEMA_MISMATCH, "pin count", item.id);
            }
            entries.push_back({item.id, &node, pins});
            placements.push_back({item.id, item.layout});
        }
        std::vector<graph::LinkRecord> links;
        links.reserve(source.links.size());
        for (const auto& link : source.links)
        {
            links.push_back({link.from, link.to});
        }
        FlowGraphChange change;
        change.insert = entries;
        change.connect = links;
        change.place = placements;
        auto edit = FlowGraphEdit::prepare(graph, change);
        if (!edit)
        {
            FlowSourceFailure error{EFlowSourceError::INVALID_TOPOLOGY, "graph"};
            std::visit(
                [&]<class T>(const T& failure) noexcept
                {
                    if constexpr (std::is_same_v<T, graph::GraphTopologyFailure>)
                    {
                        error.node = failure.node;
                        error.pin = failure.pin;
                    }
                    else if constexpr (std::is_same_v<T, FlowForgeFailure>)
                    {
                        error.node = NodeId{failure.node_id};
                        error.pin = PinId{failure.pin_id};
                        error.cause = failure;
                    }
                    else
                    {
                        error.code = EFlowSourceError::UNSUPPORTED_LITERAL;
                    }
                },
                edit.error()
            );
            return cxx::unexpected(std::move(error));
        }
        edit->commit();
        for (const auto& item : source.exports)
        {
            if (!graph.addExport({FlowForgeExportNodeId{item.id}, item.entry, item.symbol, item.hints}))
            {
                return fail(EFlowSourceError::INVALID_VALUE, "export", item.entry);
            }
        }
        return graph;
    }
} // namespace lux::flowforge
