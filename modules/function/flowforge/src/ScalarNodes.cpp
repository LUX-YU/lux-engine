#include <lux/engine/flowforge/ScalarNodes.hpp>
#include <lux/engine/flowforge/detail/FlowSourceMetadata.hpp>

#include <array>
#include <lux/engine/flowforge/detail/FlowPinSchema.hpp>
#include <lux/engine/flowforge/detail/ScalarLowering.hpp>

namespace lux::flowforge
{
    using detail::EScalarOperation;

    namespace
    {
        FlowForgeFailure invalidScalar(std::string message) noexcept
        {
            return {EFlowForgeError::GRAPH_INVALID, std::move(message)};
        }

        FlowForgeResult<std::unique_ptr<ScalarNodePayload>> cloneScalar(const ScalarNodePayload& value) noexcept
        {
            return std::make_unique<ScalarNodePayload>(value);
        }

        template <EScalarOperation Operation>
        FlowForgeResult<FlowNodePayload> createScalar(const object::CodeLease& code) noexcept
        {
            constexpr bool is_logical = Operation == EScalarOperation::LOGICAL_AND ||
                                        Operation == EScalarOperation::LOGICAL_OR ||
                                        Operation == EScalarOperation::LOGICAL_NOT;
            if constexpr (is_logical)
            {
                return FlowNodePayload::make<ScalarNodePayload, cloneScalar>(code, &meta::ref_type_of_v<bool>);
            }
            return FlowNodePayload::make<ScalarNodePayload, cloneScalar>(code, &meta::ref_type_of_v<std::int32_t>);
        }

        FlowForgeResult<void> validateScalar(const FlowNodePayload& payload) noexcept
        {
            const auto* value = payload.get<ScalarNodePayload>();
            const bool has_type = value && value->operand_type;
            if (!has_type)
            {
                return cxx::unexpected(invalidScalar("scalar node has no operand type"));
            }
            return {};
        }

        template <EScalarOperation Operation> constexpr bool isUnary() noexcept
        {
            return Operation == EScalarOperation::NEGATE || Operation == EScalarOperation::LOGICAL_NOT;
        }

        template <EScalarOperation Operation> const meta::RefType* resultType(const meta::RefType* operand) noexcept
        {
            constexpr bool is_comparison =
                Operation >= EScalarOperation::CMP_EQ && Operation <= EScalarOperation::CMP_GE;
            constexpr bool is_logical = Operation == EScalarOperation::LOGICAL_AND ||
                                        Operation == EScalarOperation::LOGICAL_OR ||
                                        Operation == EScalarOperation::LOGICAL_NOT;
            if constexpr (is_comparison || is_logical)
            {
                return &meta::ref_type_of_v<bool>;
            }
            return operand;
        }

        template <EScalarOperation Operation>
        FlowNodeRegistration::PinResult describeScalar(const FlowNodePayload& payload) noexcept
        {
            auto valid = validateScalar(payload);
            if (!valid)
            {
                return cxx::unexpected(std::move(valid.error()));
            }
            const auto* type = payload.get<ScalarNodePayload>()->operand_type;
            std::vector<FlowPinDeclaration> pins;
            detail::appendDataPin(pins, graph::EPinDirection::INPUT, 0, "A", type, true);
            if constexpr (!isUnary<Operation>())
            {
                detail::appendDataPin(pins, graph::EPinDirection::INPUT, 1, "B", type, true);
            }
            detail::appendDataPin(pins, graph::EPinDirection::OUTPUT, 0, "Result", resultType<Operation>(type));
            return pins;
        }

        template <EScalarOperation Operation>
        FlowNodeRegistration::ValueResult compileScalar(
            const FlowNodePayload& payload,
            std::span<const FlowValue> inputs,
            FlowValueCompiler& compiler
        ) noexcept
        {
            const auto& type = *payload.get<ScalarNodePayload>()->operand_type;
            const auto instruction = isUnary<Operation>() ? detail::selectUnaryScalarInstruction(Operation, type)
                                                          : detail::selectBinaryScalarInstruction(Operation, type);
            if (!instruction)
            {
                return cxx::unexpected(invalidScalar("not a scalar operation"));
            }
            auto result = compiler.emitScalar(*instruction, inputs);
            if (!result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
            return std::vector<FlowValue>{*result};
        }

        FlowSourceResult<VFlowSourceParameters> captureScalar(const FlowNodePayload& payload) noexcept
        {
            return FlowSourceType{std::string(payload.get<ScalarNodePayload>()->operand_type->name)};
        }

        FlowSourceResult<FlowNodePayload> restoreScalar(
            const FlowSourceNode& source,
            const FlowSourceEnvironment& environment,
            FlowReferenceView,
            const object::CodeLease& code
        ) noexcept
        {
            const auto* saved = std::get_if<FlowSourceType>(&source.parameters);
            if (!saved)
            {
                return detail::sourceFailure(EFlowSourceError::SCHEMA_MISMATCH, source.type, source.id);
            }
            const auto* type = detail::findSourceType(saved->name, environment);
            if (!type)
            {
                return detail::sourceFailure(EFlowSourceError::UNKNOWN_TYPE, saved->name, source.id);
            }
            return detail::sourcePayload<ScalarNodePayload, cloneScalar>(code, type);
        }

        template <EScalarOperation Operation>
        FlowNodeRegistration registration(std::string_view name, const object::CodeLease& code) noexcept
        {
            return {
                {graph::nodeTypeId(name), std::string(name), 1},
                cxx::typeToken<ScalarNodePayload>(),
                code,
                createScalar<Operation>,
                describeScalar<Operation>,
                validateScalar,
                compileScalar<Operation>,
                captureScalar,
                restoreScalar
            };
        }
    } // namespace

    std::vector<FlowNodeRegistration> scalarNodeRegistrations(object::CodeLease code) noexcept
    {
        return {
            registration<EScalarOperation::ADD>("lux.flow.add", code),
            registration<EScalarOperation::SUBTRACT>("lux.flow.subtract", code),
            registration<EScalarOperation::MULTIPLY>("lux.flow.multiply", code),
            registration<EScalarOperation::DIVIDE>("lux.flow.divide", code),
            registration<EScalarOperation::MODULO>("lux.flow.modulo", code),
            registration<EScalarOperation::LOGICAL_AND>("lux.flow.and", code),
            registration<EScalarOperation::LOGICAL_OR>("lux.flow.or", code),
            registration<EScalarOperation::LOGICAL_NOT>("lux.flow.not", code),
            registration<EScalarOperation::NEGATE>("lux.flow.negate", code),
            registration<EScalarOperation::CMP_EQ>("lux.flow.equal", code),
            registration<EScalarOperation::CMP_NE>("lux.flow.not_equal", code),
            registration<EScalarOperation::CMP_LT>("lux.flow.less", code),
            registration<EScalarOperation::CMP_LE>("lux.flow.less_equal", code),
            registration<EScalarOperation::CMP_GT>("lux.flow.greater", code),
            registration<EScalarOperation::CMP_GE>("lux.flow.greater_equal", code)
        };
    }
} // namespace lux::flowforge
