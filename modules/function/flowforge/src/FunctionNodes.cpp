#include <lux/engine/flowforge/FlowExecutionCompiler.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/detail/FlowPinSchema.hpp>
#include <lux/engine/flowforge/graph/FlowNode.hpp>

#include <type_traits>

namespace lux::flowforge
{
    namespace
    {
        template <class T> FlowForgeResult<std::unique_ptr<T>> cloneFunction(const T& value) noexcept
        {
            return std::make_unique<T>(value);
        }

        template <class T>
        FlowNodeRegistration functionRegistration(std::string name, const object::CodeLease& code) noexcept
        {
            FlowNodeRegistration result;
            result.identity = {graph::nodeTypeId(name), std::move(name), 1};
            result.payload_type = cxx::typeToken<T>();
            result.code = code;
            result.create = [](const object::CodeLease& lease) noexcept
            { return FlowNodePayload::make<T, cloneFunction<T>>(lease); };
            result.describe_pins = [](const FlowNodePayload& payload) noexcept
            { return payload.get<T>()->describePins(); };
            result.validate = [](const FlowNodePayload& payload) noexcept -> FlowForgeResult<void>
            {
                auto schema = payload.get<T>()->describePins();
                if (!schema)
                {
                    return cxx::unexpected(std::move(schema.error()));
                }
                return {};
            };
            if constexpr (std::is_same_v<T, FunctionCallPayload> || std::is_same_v<T, FunctionReturnPayload>)
            {
                result.validate_references = [](const FlowNodePayload& payload, FlowReferenceView view) noexcept
                { return payload.get<T>()->validateReferences(view); };
            }
            result.compile_execution = [](const FlowNodePayload& payload,
                                          FlowExecutionCompiler& compiler) noexcept -> FlowForgeResult<void>
            {
                if constexpr (std::is_same_v<T, FunctionCallPayload>)
                {
                    const auto& call = *payload.get<T>();
                    const auto arguments =
                        detail::parameterSemantics(graph::EPinDirection::INPUT, call.arguments.size());
                    const auto results = detail::parameterSemantics(graph::EPinDirection::OUTPUT, call.results.size());
                    return compiler.functionCall(
                        call.callee,
                        arguments,
                        results,
                        detail::pinSemantic(EFlowPinRole::EXECUTION, graph::EPinDirection::OUTPUT, 0)
                    );
                }
                else if constexpr (std::is_same_v<T, FunctionReturnPayload>)
                {
                    const auto values =
                        detail::parameterSemantics(graph::EPinDirection::INPUT, payload.get<T>()->results.size());
                    return compiler.returnValues(values);
                }
                else
                {
                    static_assert(std::is_same_v<T, FunctionPayload> || std::is_same_v<T, EventEntryPayload>);
                    return cxx::unexpected(
                        FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "entry node reached mid-chain"}
                    );
                }
            };
            return result;
        }

        FlowForgeResult<std::unique_ptr<NativeCallPayload>> cloneNativeCall(const NativeCallPayload& value) noexcept
        {
            return std::make_unique<NativeCallPayload>(value);
        }

        [[nodiscard]] bool matchesParameters(
            std::span<const FuncArgInfo> actual,
            std::span<const FuncArgInfo> expected
        ) noexcept
        {
            if (actual.size() != expected.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < actual.size(); ++i)
            {
                const bool has_types = actual[i].type && expected[i].type;
                const bool is_type_mismatch = !has_types || *actual[i].type != *expected[i].type;
                if (is_type_mismatch)
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] FlowForgeResult<void> appendParameters(
            std::vector<FlowPinDeclaration>& pins,
            std::span<const FuncArgInfo> parameters,
            graph::EPinDirection direction
        ) noexcept
        {
            for (std::size_t i = 0; i < parameters.size(); ++i)
            {
                const auto& parameter = parameters[i];
                if (!parameter.type)
                {
                    return cxx::unexpected(
                        FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "function parameter has no type"}
                    );
                }
                detail::appendDataPin(
                    pins,
                    direction,
                    i + 1,
                    parameter.name,
                    parameter.type,
                    direction == graph::EPinDirection::INPUT
                );
            }
            return {};
        }
    } // namespace

    FlowNodeRegistration::PinResult FunctionPayload::describePins() const noexcept
    {
        std::vector<FlowPinDeclaration> pins;
        detail::appendExecutionPin(pins, graph::EPinDirection::OUTPUT, "->");
        if (auto added = appendParameters(pins, arguments, graph::EPinDirection::OUTPUT); !added)
        {
            return cxx::unexpected(std::move(added.error()));
        }
        for (const auto& result : results)
        {
            if (!result.type)
            {
                return cxx::unexpected(
                    FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "function result has no type"}
                );
            }
        }
        return pins;
    }

    bool FunctionReturnPayload::matchesSignature(const FunctionPayload& signature) const noexcept
    {
        return matchesParameters(results, signature.results);
    }

    FlowForgeResult<void> FunctionReturnPayload::validateReferences(FlowReferenceView references) const noexcept
    {
        const auto* node = references.node(definition);
        const auto* signature = node ? node->payload.get<FunctionPayload>() : nullptr;
        if (!signature)
        {
            return cxx::unexpected(FlowForgeFailure{
                EFlowForgeError::GRAPH_INVALID,
                "function return references a definition outside the graph"
            });
        }
        if (!matchesSignature(*signature))
        {
            return cxx::unexpected(FlowForgeFailure{
                EFlowForgeError::GRAPH_INVALID,
                "function return signature differs from its definition"
            });
        }
        return {};
    }

    FlowNodeRegistration::PinResult FunctionReturnPayload::describePins() const noexcept
    {
        std::vector<FlowPinDeclaration> pins;
        detail::appendExecutionPin(pins, graph::EPinDirection::INPUT, "->");
        if (auto added = appendParameters(pins, results, graph::EPinDirection::INPUT); !added)
        {
            return cxx::unexpected(std::move(added.error()));
        }
        return pins;
    }

    bool FunctionCallPayload::matchesSignature(const FunctionPayload& signature) const noexcept
    {
        return matchesParameters(arguments, signature.arguments) && matchesParameters(results, signature.results);
    }

    FlowForgeResult<void> FunctionCallPayload::validateReferences(FlowReferenceView references) const noexcept
    {
        const auto* node = references.node(callee);
        const auto* signature = node ? node->payload.get<FunctionPayload>() : nullptr;
        if (!signature)
        {
            return cxx::unexpected(FlowForgeFailure{
                EFlowForgeError::GRAPH_INVALID,
                "graph function call references a definition outside the graph"
            });
        }
        if (!matchesSignature(*signature))
        {
            return cxx::unexpected(FlowForgeFailure{
                EFlowForgeError::GRAPH_INVALID,
                "graph function call signature differs from its definition"
            });
        }
        return {};
    }

    FlowNodeRegistration::PinResult FunctionCallPayload::describePins() const noexcept
    {
        std::vector<FlowPinDeclaration> pins;
        detail::appendExecutionPin(pins, graph::EPinDirection::INPUT, "->");
        if (auto added = appendParameters(pins, arguments, graph::EPinDirection::INPUT); !added)
        {
            return cxx::unexpected(std::move(added.error()));
        }
        detail::appendExecutionPin(pins, graph::EPinDirection::OUTPUT, "->");
        if (auto added = appendParameters(pins, results, graph::EPinDirection::OUTPUT); !added)
        {
            return cxx::unexpected(std::move(added.error()));
        }
        return pins;
    }

    FlowNodeRegistration::PinResult EventEntryPayload::describePins() const noexcept
    {
        std::vector<FlowPinDeclaration> pins;
        detail::appendExecutionPin(pins, graph::EPinDirection::OUTPUT, "->");
        if (auto added = appendParameters(pins, parameters, graph::EPinDirection::OUTPUT); !added)
        {
            return cxx::unexpected(std::move(added.error()));
        }
        return pins;
    }

    FlowNodeRegistration::PinResult NativeCallPayload::describePins() const noexcept
    {
        if (!definition)
        {
            return cxx::unexpected(
                FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "native call has no definition"}
            );
        }
        std::vector<FlowPinDeclaration> pins;
        detail::appendExecutionPin(pins, graph::EPinDirection::INPUT, "->");
        const auto& signature = definition->signature();
        for (std::size_t i = 0; i < signature.parameters.size(); ++i)
        {
            const auto& parameter = signature.parameters[i];
            detail::appendDataPin(
                pins,
                graph::EPinDirection::INPUT,
                i + 1,
                std::string(parameter.name),
                &parameter.type,
                true
            );
        }
        if (const auto* receiver = definition->receiver())
        {
            detail::appendDataPin(pins, graph::EPinDirection::INPUT, signature.parameters.size() + 1, "Self", receiver);
        }
        detail::appendExecutionPin(pins, graph::EPinDirection::OUTPUT, "->");
        detail::appendDataPin(pins, graph::EPinDirection::OUTPUT, 1, "Return", &signature.return_type);
        return pins;
    }

    FlowForgeResult<std::vector<graph::PinSemanticId>> NativeCallPayload::argumentSemantics() const noexcept
    {
        std::vector<graph::PinSemanticId> result;
        if (!definition)
        {
            return cxx::unexpected(
                FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "native call has no definition"}
            );
        }
        const auto count = definition->signature().parameters.size();
        if (definition->receiver())
        {
            result.push_back(detail::pinSemantic(EFlowPinRole::DATA, graph::EPinDirection::INPUT, count + 1));
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            result.push_back(detail::pinSemantic(EFlowPinRole::DATA, graph::EPinDirection::INPUT, i + 1));
        }
        return result;
    }

    std::vector<FlowNodeRegistration> functionNodeRegistrations(object::CodeLease code) noexcept
    {
        return {
            functionRegistration<FunctionPayload>("lux.flow.function", code),
            functionRegistration<FunctionReturnPayload>("lux.flow.function_return", code),
            functionRegistration<FunctionCallPayload>("lux.flow.function_call", code),
            functionRegistration<EventEntryPayload>("lux.flow.event", code)
        };
    }

    FlowNodeRegistration nativeCallRegistration(object::CodeLease code) noexcept
    {
        FlowNodeRegistration result;
        constexpr std::string_view name = "lux.flow.native_call";
        result.identity = {graph::nodeTypeId(name), std::string(name), 1};
        result.payload_type = cxx::typeToken<NativeCallPayload>();
        result.code = std::move(code);
        result.create = [](const object::CodeLease& lease) noexcept
        { return FlowNodePayload::make<NativeCallPayload, cloneNativeCall>(lease); };
        result.describe_pins = [](const FlowNodePayload& payload) noexcept
        { return payload.get<NativeCallPayload>()->describePins(); };
        result.validate = [](const FlowNodePayload& payload) noexcept -> FlowForgeResult<void>
        {
            if (!payload.get<NativeCallPayload>()->definition)
            {
                return cxx::unexpected(
                    FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "native call has no definition"}
                );
            }
            return {};
        };
        result.compile_execution = [](const FlowNodePayload& payload,
                                      FlowExecutionCompiler& compiler) noexcept -> FlowForgeResult<void>
        {
            const auto& call = *payload.get<NativeCallPayload>();
            auto arguments = call.argumentSemantics();
            if (!arguments)
            {
                return cxx::unexpected(std::move(arguments.error()));
            }
            return compiler.nativeCall(
                *call.definition,
                *arguments,
                detail::pinSemantic(EFlowPinRole::DATA, graph::EPinDirection::OUTPUT, 1),
                detail::pinSemantic(EFlowPinRole::EXECUTION, graph::EPinDirection::OUTPUT, 0)
            );
        };
        return result;
    }
} // namespace lux::flowforge
