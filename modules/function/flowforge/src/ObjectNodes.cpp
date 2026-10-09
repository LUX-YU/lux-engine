#include <lux/engine/flowforge/FlowExecutionCompiler.hpp>
#include <lux/engine/flowforge/ObjectNodes.hpp>
#include <lux/engine/flowforge/detail/FlowPinSchema.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <type_traits>

namespace lux::flowforge
{
    namespace
    {
        using graph::EPinDirection;

        template <class T> FlowForgeResult<std::unique_ptr<T>> cloneObject(const T& value) noexcept
        {
            return std::make_unique<T>(value);
        }

        template <class T> FlowNodeRegistration registration(std::string name, const object::CodeLease& code) noexcept
        {
            FlowNodeRegistration result;
            result.identity = {graph::nodeTypeId(name), std::move(name), 1};
            result.payload_type = cxx::typeToken<T>();
            result.code = code;
            result.create = [](const object::CodeLease& lease) noexcept
            { return FlowNodePayload::make<T, cloneObject<T>>(lease); };
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
            if constexpr (std::is_same_v<T, GetVariablePayload> || std::is_same_v<T, SetVariablePayload>)
            {
                result.validate_references = [](const FlowNodePayload& payload, FlowReferenceView view) noexcept
                { return payload.get<T>()->validateReferences(view); };
            }
            constexpr bool reads_state = std::is_same_v<T, GetFieldPayload> || std::is_same_v<T, GetVariablePayload>;
            if constexpr (reads_state || std::is_same_v<T, GetObjectPayload>)
            {
                result.value_evaluation = reads_state ? EFlowValueEvaluation::READS_STATE : EFlowValueEvaluation::PURE;
                result.compile = [](const FlowNodePayload& payload,
                                    std::span<const FlowValue> inputs,
                                    FlowValueCompiler& compiler) noexcept -> FlowNodeRegistration::ValueResult
                {
                    if constexpr (std::is_same_v<T, GetObjectPayload>)
                    {
                        // Preserve the original compiler's unsupported operation, not a dummy value.
                        return cxx::unexpected(FlowForgeFailure{
                            EFlowForgeError::GRAPH_INVALID,
                            "no pure lowering registered for this node"
                        });
                    }
                    else
                    {
                        auto value = [&]() noexcept -> FlowForgeResult<FlowValue>
                        {
                            if constexpr (std::is_same_v<T, GetFieldPayload>)
                            {
                                return compiler.readField(*payload.get<T>()->field, inputs.front());
                            }
                            else
                            {
                                return compiler.readVariable(payload.get<T>()->variable);
                            }
                        }();
                        if (!value)
                        {
                            return cxx::unexpected(std::move(value.error()));
                        }
                        return std::vector<FlowValue>{*value};
                    }
                };
            }
            else
            {
                result.compile_execution = [](const FlowNodePayload& payload,
                                              FlowExecutionCompiler& compiler) noexcept -> FlowForgeResult<void>
                {
                    const auto completed = detail::pinSemantic(EFlowPinRole::EXECUTION, EPinDirection::OUTPUT, 0);
                    const auto result = detail::pinSemantic(EFlowPinRole::DATA, EPinDirection::OUTPUT, 1);
                    if constexpr (std::is_same_v<T, SetVariablePayload>)
                    {
                        return compiler.storeVariable(
                            payload.get<T>()->variable,
                            detail::pinSemantic(EFlowPinRole::DATA, EPinDirection::INPUT, 1),
                            result,
                            completed
                        );
                    }
                    else if constexpr (std::is_same_v<T, SetFieldPayload>)
                    {
                        return compiler.storeField(
                            *payload.get<T>()->field,
                            detail::pinSemantic(EFlowPinRole::DATA, EPinDirection::INPUT, 1),
                            detail::pinSemantic(EFlowPinRole::DATA, EPinDirection::INPUT, 2),
                            result,
                            completed
                        );
                    }
                    else
                    {
                        static_assert(std::is_same_v<T, SetObjectPayload>);
                        return cxx::unexpected(
                            FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "no lowering registered"}
                        );
                    }
                };
            }
            return result;
        }

        auto missingType() noexcept
        {
            return cxx::unexpected(FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "object node has no type"});
        }

        auto missingField() noexcept
        {
            return cxx::unexpected(
                FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "field node has no reflected owner or field"}
            );
        }

        FlowForgeResult<void> validateVariable(
            std::uint64_t id,
            const meta::RefType* type,
            FlowReferenceView references
        ) noexcept
        {
            const auto* actual = references.variable_type(id);
            if (!actual)
            {
                return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "graph variable not found"});
            }
            const bool is_type_mismatch = !type || *actual != *type;
            if (is_type_mismatch)
            {
                return cxx::unexpected(
                    FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "variable node type differs from its definition"}
                );
            }
            return {};
        }

        void appendObjectInput(
            std::vector<FlowPinDeclaration>& pins,
            std::size_t ordinal,
            const meta::RefClass& owner
        ) noexcept
        {
            detail::appendDataPin(pins, EPinDirection::INPUT, ordinal, "Object", &owner.type);
            // Mandatory external object reference: preserve the old explicit empty RuntimeObject.
            pins.back().initial_value = nullptr;
        }
    } // namespace

    std::vector<FlowNodeRegistration> objectNodeRegistrations(object::CodeLease code) noexcept
    {
        return {
            registration<GetObjectPayload>("lux.flow.get_object", code),
            registration<SetObjectPayload>("lux.flow.set_object", code),
            registration<GetFieldPayload>("lux.flow.get_field", code),
            registration<SetFieldPayload>("lux.flow.set_field", code),
            registration<GetVariablePayload>("lux.flow.get_variable", code),
            registration<SetVariablePayload>("lux.flow.set_variable", code)
        };
    }

    FlowNodeRegistration::PinResult GetObjectPayload::describePins() const noexcept
    {
        if (!type)
        {
            return missingType();
        }
        std::vector<FlowPinDeclaration> pins;
        detail::appendDataPin(pins, EPinDirection::OUTPUT, 0, "Value", type);
        return pins;
    }

    FlowNodeRegistration::PinResult SetObjectPayload::describePins() const noexcept
    {
        if (!type)
        {
            return missingType();
        }
        std::vector<FlowPinDeclaration> pins;
        detail::appendExecutionPin(pins, EPinDirection::INPUT, "->");
        detail::appendDataPin(pins, EPinDirection::INPUT, 1, "Value", type);
        detail::appendExecutionPin(pins, EPinDirection::OUTPUT, "Completed");
        detail::appendDataPin(pins, EPinDirection::OUTPUT, 1, "Object Out", type);
        return pins;
    }

    FlowNodeRegistration::PinResult GetFieldPayload::describePins() const noexcept
    {
        const bool has_metadata = owner != nullptr && field != nullptr;
        if (!has_metadata)
        {
            return missingField();
        }
        std::vector<FlowPinDeclaration> pins;
        appendObjectInput(pins, 0, *owner);
        detail::appendDataPin(pins, EPinDirection::OUTPUT, 0, std::string(field->name), &field->type);
        return pins;
    }

    FlowNodeRegistration::PinResult SetFieldPayload::describePins() const noexcept
    {
        const bool has_metadata = owner != nullptr && field != nullptr;
        if (!has_metadata)
        {
            return missingField();
        }
        std::vector<FlowPinDeclaration> pins;
        detail::appendExecutionPin(pins, EPinDirection::INPUT, "->");
        appendObjectInput(pins, 1, *owner);
        detail::appendDataPin(pins, EPinDirection::INPUT, 2, std::string(field->name), &field->type, true);
        detail::appendExecutionPin(pins, EPinDirection::OUTPUT, "->");
        detail::appendDataPin(pins, EPinDirection::OUTPUT, 1, "Object", &owner->type);
        return pins;
    }

    FlowNodeRegistration::PinResult GetVariablePayload::describePins() const noexcept
    {
        if (!type)
        {
            return missingType();
        }
        std::vector<FlowPinDeclaration> pins;
        detail::appendDataPin(pins, EPinDirection::OUTPUT, 0, "Value", type);
        return pins;
    }

    FlowForgeResult<void> GetVariablePayload::validateReferences(FlowReferenceView references) const noexcept
    {
        return validateVariable(variable, type, references);
    }

    FlowNodeRegistration::PinResult SetVariablePayload::describePins() const noexcept
    {
        if (!type)
        {
            return missingType();
        }
        std::vector<FlowPinDeclaration> pins;
        detail::appendExecutionPin(pins, EPinDirection::INPUT, "->");
        detail::appendDataPin(pins, EPinDirection::INPUT, 1, "Value", type, true);
        detail::appendExecutionPin(pins, EPinDirection::OUTPUT, "->");
        detail::appendDataPin(pins, EPinDirection::OUTPUT, 1, "Value", type);
        return pins;
    }

    FlowForgeResult<void> SetVariablePayload::validateReferences(FlowReferenceView references) const noexcept
    {
        return validateVariable(variable, type, references);
    }
} // namespace lux::flowforge
