#include <lux/engine/material/MaterialNodeCatalog.hpp>
#include <lux/engine/material/detail/MaterialMath.hpp>

#include <array>

namespace lux::material
{
    namespace
    {
        struct MathOperation final
        {
            shadergen::EOp op;
            std::uint8_t inputs;
            bool scalar_result{};
        };

        // Same ordinal contract as EMathOp. Zero inputs denotes the unsupported legacy LERP value.
        constexpr std::array Operations{
            MathOperation{shadergen::EOp::MUL, 2},         MathOperation{shadergen::EOp::ADD, 2},
            MathOperation{shadergen::EOp::SUB, 2},         MathOperation{shadergen::EOp::DIV, 2},
            MathOperation{shadergen::EOp::DOT, 2, true},   MathOperation{shadergen::EOp::MIN, 2},
            MathOperation{shadergen::EOp::MAX, 2},         MathOperation{shadergen::EOp::POW, 2},
            MathOperation{shadergen::EOp::STEP, 2},        MathOperation{shadergen::EOp::MOD, 2},
            MathOperation{shadergen::EOp::CROSS, 2},       MathOperation{shadergen::EOp::REFLECT, 2},
            MathOperation{shadergen::EOp::LERP, 0},        MathOperation{shadergen::EOp::SATURATE, 1},
            MathOperation{shadergen::EOp::ONE_MINUS, 1},   MathOperation{shadergen::EOp::ABS, 1},
            MathOperation{shadergen::EOp::SQRT, 1},        MathOperation{shadergen::EOp::FLOOR, 1},
            MathOperation{shadergen::EOp::FRACT, 1},       MathOperation{shadergen::EOp::SIN, 1},
            MathOperation{shadergen::EOp::COS, 1},         MathOperation{shadergen::EOp::NORMALIZE, 1},
            MathOperation{shadergen::EOp::LENGTH, 1, true}
        };

        static_assert(Operations.size() == static_cast<std::size_t>(EMathOp::LENGTH) + 1);

        const MathOperation* operation(EMathOp op) noexcept
        {
            const auto index = static_cast<std::size_t>(op);
            return index < Operations.size() ? &Operations[index] : nullptr;
        }

        MaterialCompileFailure invalid(std::string message) noexcept
        {
            return {EMaterialCompileError::INVALID_GRAPH, std::move(message)};
        }

        MaterialNodeResult<std::unique_ptr<MaterialMath>> cloneMath(const MaterialMath& source) noexcept
        {
            return std::make_unique<MaterialMath>(source);
        }
    } // namespace

    std::size_t detail::mathInputCount(EMathOp op) noexcept
    {
        const auto* info = operation(op);
        return info ? info->inputs : 0;
    }

    EValueType detail::mathOutputType(const MaterialMath& value) noexcept
    {
        const auto* info = operation(value.op);
        return info && info->scalar_result ? EValueType::FLOAT : value.operand_type;
    }

    cxx::expected<void, MaterialCompileFailure> detail::validateMathPayload(const MaterialMath& value) noexcept
    {
        const bool is_invalid_operation = operation(value.op) == nullptr;
        const bool is_invalid_type = value.operand_type < EValueType::FLOAT || value.operand_type > EValueType::VEC4;
        const bool is_invalid_math = is_invalid_operation || is_invalid_type;
        if (is_invalid_math)
        {
            return cxx::unexpected(invalid("invalid or unsupported Math node payload"));
        }
        return {};
    }

    cxx::expected<void, MaterialCompileFailure> detail::validateMath(const MaterialMath& value) noexcept
    {
        auto payload = validateMathPayload(value);
        if (!payload)
        {
            return payload;
        }
        if (operation(value.op)->inputs == 0)
        {
            return cxx::unexpected(invalid("invalid or unsupported Math node payload"));
        }
        const bool requires_vector = value.op == EMathOp::DOT || value.op == EMathOp::CROSS;
        const bool is_scalar_mismatch = requires_vector && value.operand_type == EValueType::FLOAT;
        if (is_scalar_mismatch)
        {
            return cxx::unexpected(invalid("Dot/Cross require vector operands"));
        }
        const bool is_cross_mismatch = value.op == EMathOp::CROSS && value.operand_type != EValueType::VEC3;
        if (is_cross_mismatch)
        {
            return cxx::unexpected(invalid("Cross requires Vec3 operands"));
        }
        return {};
    }

    cxx::expected<std::uint32_t, MaterialCompileFailure> detail::appendMath(
        const MaterialMath& value,
        std::span<const std::uint32_t> inputs,
        shadergen::ShaderIR& ir
    ) noexcept
    {
        auto validation = validateMath(value);
        if (!validation)
        {
            return cxx::unexpected(std::move(validation.error()));
        }
        if (inputs.size() != 2)
        {
            return cxx::unexpected(invalid("Math requires two authoring input positions"));
        }
        const auto& info = *operation(value.op);
        for (std::size_t index = 0; index != inputs.size(); ++index)
        {
            if (index >= info.inputs)
            {
                if (inputs[index] != shadergen::kNoValue)
                {
                    return cxx::unexpected(invalid("unused Math input must not be evaluated"));
                }
                continue;
            }
            const bool has_value = inputs[index] < ir.values.size();
            const bool is_type_mismatch =
                has_value && ir.values[inputs[index]].type != static_cast<shadergen::EValueType>(value.operand_type);
            if (!has_value)
            {
                return cxx::unexpected(invalid("Math input has no SSA value"));
            }
            if (is_type_mismatch)
            {
                return cxx::unexpected(MaterialCompileFailure{
                    EMaterialCompileError::TYPE_MISMATCH,
                    "Math input type differs from its declared operand type"
                });
            }
        }
        shadergen::ShaderIRValue expression{};
        expression.op = info.op;
        expression.type = static_cast<shadergen::EValueType>(mathOutputType(value));
        for (std::size_t index = 0; index != info.inputs; ++index)
        {
            expression.operands[index] = inputs[index];
        }
        const auto result = static_cast<std::uint32_t>(ir.values.size());
        ir.values.push_back(expression);
        return result;
    }

    MaterialNodeRegistration materialMathRegistration() noexcept
    {
        MaterialNodeRegistration result;
        std::string name{"lux.material.math.v1"};
        result.identity = {graph::nodeTypeId(name), std::move(name), 1};
        result.payload_type = cxx::typeToken<MaterialMath>();
        result.create = [](const object::CodeLease& code) noexcept
        { return MaterialNodePayload::make<MaterialMath, &cloneMath>(code); };
        result.validate = [](const MaterialNodePayload& value) noexcept
        { return detail::validateMath(*value.get<MaterialMath>()); };
        result.describe_pins = [](const MaterialNodePayload& value) noexcept -> MaterialNodeRegistration::PinResult
        {
            const auto& math = *value.get<MaterialMath>();
            auto validated = detail::validateMathPayload(math);
            if (!validated)
            {
                return cxx::unexpected(std::move(validated.error()));
            }
            const auto second_use =
                detail::mathInputCount(math.op) == 1 ? EMaterialInputUse::UNUSED : EMaterialInputUse::VALUE;
            constexpr graph::PinSemanticId ResultSemantic{(std::uint64_t{1} << 63) | 1};
            const auto output_type = detail::mathOutputType(math);
            return std::vector<MaterialPinDeclaration>{
                {graph::PinSemanticId{1}, "a", graph::EPinDirection::INPUT, math.operand_type},
                {graph::PinSemanticId{2}, "b", graph::EPinDirection::INPUT, math.operand_type, {}, second_use},
                {ResultSemantic, "result", graph::EPinDirection::OUTPUT, output_type}
            };
        };
        result.compile = [](const MaterialNodePayload& value,
                            std::span<const std::uint32_t> inputs,
                            shadergen::ShaderIR& ir) noexcept -> MaterialNodeRegistration::ShaderResult
        {
            auto output = detail::appendMath(*value.get<MaterialMath>(), inputs, ir);
            if (!output)
            {
                return cxx::unexpected(std::move(output.error()));
            }
            return std::vector<std::uint32_t>{*output};
        };
        return result;
    }
} // namespace lux::material
