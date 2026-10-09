#include <lux/engine/flowforge/ScalarNodes.hpp>
#include <lux/engine/flowforge/detail/FlowNodeIdentity.hpp>
#include <lux/engine/flowforge/detail/ScalarLowering.hpp>

namespace lux::flowforge
{
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

        template <ENodeOperation Operation>
        FlowForgeResult<FlowNodePayload> createScalar(const object::CodeLease& code) noexcept
        {
            constexpr bool is_logical = Operation == ENodeOperation::LOGICAL_AND ||
                                        Operation == ENodeOperation::LOGICAL_OR ||
                                        Operation == ENodeOperation::LOGICAL_NOT;
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

        template <ENodeOperation Operation> constexpr bool isUnary() noexcept
        {
            return Operation == ENodeOperation::NEGATE || Operation == ENodeOperation::LOGICAL_NOT;
        }

        template <ENodeOperation Operation> const meta::RefType* resultType(const meta::RefType* operand) noexcept
        {
            constexpr bool is_comparison = Operation >= ENodeOperation::CMP_EQ && Operation <= ENodeOperation::CMP_GE;
            constexpr bool is_logical = Operation == ENodeOperation::LOGICAL_AND ||
                                        Operation == ENodeOperation::LOGICAL_OR ||
                                        Operation == ENodeOperation::LOGICAL_NOT;
            if constexpr (is_comparison || is_logical)
            {
                return &meta::ref_type_of_v<bool>;
            }
            return operand;
        }

        template <ENodeOperation Operation>
        FlowNodeRegistration::PinResult describeScalar(const FlowNodePayload& payload) noexcept
        {
            auto valid = validateScalar(payload);
            if (!valid)
            {
                return cxx::unexpected(std::move(valid.error()));
            }
            const auto* type = payload.get<ScalarNodePayload>()->operand_type;
            std::vector<FlowPinDeclaration> pins{
                {detail::builtinPinSemantic(EPinKind::DATA_IN, 0), "A", graph::EPinDirection::INPUT, type, true}
            };
            if constexpr (!isUnary<Operation>())
            {
                pins.push_back(
                    {detail::builtinPinSemantic(EPinKind::DATA_IN, 1), "B", graph::EPinDirection::INPUT, type, true}
                );
            }
            pins.push_back(
                {detail::builtinPinSemantic(EPinKind::DATA_OUT, 0),
                 "Result",
                 graph::EPinDirection::OUTPUT,
                 resultType<Operation>(type)}
            );
            return pins;
        }

        template <ENodeOperation Operation>
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

        FlowForgeResult<std::string> encodeScalar(const FlowNodePayload& payload) noexcept
        {
            return std::string(payload.get<ScalarNodePayload>()->operand_type->name);
        }

        FlowForgeResult<FlowNodePayload> decodeScalar(std::string_view name, const object::CodeLease& code) noexcept
        {
            const std::array types{
                &meta::ref_type_of_v<bool>,
                &meta::ref_type_of_v<std::int8_t>,
                &meta::ref_type_of_v<std::uint8_t>,
                &meta::ref_type_of_v<std::int16_t>,
                &meta::ref_type_of_v<std::uint16_t>,
                &meta::ref_type_of_v<std::int32_t>,
                &meta::ref_type_of_v<std::uint32_t>,
                &meta::ref_type_of_v<std::int64_t>,
                &meta::ref_type_of_v<std::uint64_t>,
                &meta::ref_type_of_v<float>,
                &meta::ref_type_of_v<double>
            };
            for (const auto* type : types)
            {
                if (type->name == name)
                {
                    return FlowNodePayload::make<ScalarNodePayload, cloneScalar>(code, type);
                }
            }
            return cxx::unexpected(invalidScalar("unknown scalar operand type"));
        }

        template <ENodeOperation Operation> FlowNodeRegistration registration(const object::CodeLease& code) noexcept
        {
            const auto name = detail::builtinNodeName(Operation);
            return {
                {graph::nodeTypeId(name), std::string(name), 1},
                cxx::typeToken<ScalarNodePayload>(),
                code,
                createScalar<Operation>,
                describeScalar<Operation>,
                validateScalar,
                compileScalar<Operation>,
                encodeScalar,
                decodeScalar
            };
        }
    } // namespace

    std::vector<FlowNodeRegistration> scalarNodeRegistrations(object::CodeLease code) noexcept
    {
        return {
            registration<ENodeOperation::ADD>(code),
            registration<ENodeOperation::SUBTRACT>(code),
            registration<ENodeOperation::MULTIPLY>(code),
            registration<ENodeOperation::DIVIDE>(code),
            registration<ENodeOperation::MODULO>(code),
            registration<ENodeOperation::LOGICAL_AND>(code),
            registration<ENodeOperation::LOGICAL_OR>(code),
            registration<ENodeOperation::LOGICAL_NOT>(code),
            registration<ENodeOperation::NEGATE>(code),
            registration<ENodeOperation::CMP_EQ>(code),
            registration<ENodeOperation::CMP_NE>(code),
            registration<ENodeOperation::CMP_LT>(code),
            registration<ENodeOperation::CMP_LE>(code),
            registration<ENodeOperation::CMP_GT>(code),
            registration<ENodeOperation::CMP_GE>(code)
        };
    }
} // namespace lux::flowforge
