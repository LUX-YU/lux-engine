#include <lux/engine/material/MaterialNodeCatalog.hpp>
#include <lux/engine/material/detail/BuiltinMaterialCodec.hpp>
#include <lux/engine/material/detail/MaterialMath.hpp>

#include <array>

namespace lux::material
{
    namespace
    {
        struct MathOperation final
        {
            std::uint8_t inputs;
            bool scalar_result{};
        };

        // Same ordinal contract as EMathOp. Zero inputs denotes the unsupported legacy LERP value.
        constexpr std::array Operations{
            MathOperation{2}, MathOperation{2}, MathOperation{2},      MathOperation{2}, MathOperation{2, true},
            MathOperation{2}, MathOperation{2}, MathOperation{2},      MathOperation{2}, MathOperation{2},
            MathOperation{2}, MathOperation{2}, MathOperation{0},      MathOperation{1}, MathOperation{1},
            MathOperation{1}, MathOperation{1}, MathOperation{1},      MathOperation{1}, MathOperation{1},
            MathOperation{1}, MathOperation{1}, MathOperation{1, true}
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

    MaterialNodeRegistration materialMathRegistration() noexcept
    {
        MaterialNodeRegistration result;
        std::string name{MaterialMath::TypeName};
        result.identity = {graph::nodeTypeId(name), std::move(name), 1};
        result.payload_type = cxx::make_basic_type_info<MaterialMath>();
        detail::installBuiltinCodec<MaterialMath, &cloneMath>(result);
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
        result.emit_slang = [](const MaterialNodePayload& payload,
                               std::span<const std::string_view> inputs,
                               std::span<const std::string_view> outputs,
                               std::string& destination) noexcept -> MaterialNodeResult<void>
        {
            const auto& math = *payload.get<MaterialMath>();
            const auto a = "(" + std::string(inputs[0]) + ")";
            const auto b = "(" + std::string(inputs[1]) + ")";
            std::string expression;
            const auto call = [&](std::string_view name, bool binary = false)
            { return std::string(name) + "(" + a + (binary ? "," + b : "") + ")"; };
            switch (math.op)
            {
            case EMathOp::MUL:
                expression = a + "*" + b;
                break;
            case EMathOp::ADD:
                expression = a + "+" + b;
                break;
            case EMathOp::SUB:
                expression = a + "-" + b;
                break;
            case EMathOp::DIV:
                expression = a + "/" + b;
                break;
            case EMathOp::DOT:
                expression = call("dot", true);
                break;
            case EMathOp::MIN:
                expression = call("min", true);
                break;
            case EMathOp::MAX:
                expression = call("max", true);
                break;
            case EMathOp::POW:
                expression = call("pow", true);
                break;
            case EMathOp::STEP:
                expression = call("step", true);
                break;
            case EMathOp::MOD:
                expression = a + "-" + b + "*floor(" + a + "/" + b + ")";
                break;
            case EMathOp::CROSS:
                expression = call("cross", true);
                break;
            case EMathOp::REFLECT:
                expression = call("reflect", true);
                break;
            case EMathOp::SATURATE:
                expression = call("saturate");
                break;
            case EMathOp::ONE_MINUS:
                expression = "1.0-" + a;
                break;
            case EMathOp::ABS:
                expression = call("abs");
                break;
            case EMathOp::SQRT:
                expression = call("sqrt");
                break;
            case EMathOp::FLOOR:
                expression = call("floor");
                break;
            case EMathOp::FRACT:
                expression = call("frac");
                break;
            case EMathOp::SIN:
                expression = call("sin");
                break;
            case EMathOp::COS:
                expression = call("cos");
                break;
            case EMathOp::NORMALIZE:
                expression = call("normalize");
                break;
            case EMathOp::LENGTH:
                expression = call("length");
                break;
            default:
                return cxx::unexpected(invalid("unsupported Math operation"));
            }
            destination += std::string(outputs[0]) + " = " + expression + ";\n";
            return {};
        };
        return result;
    }
} // namespace lux::material
