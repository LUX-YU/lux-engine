#include <lux/engine/material/detail/BuiltinMaterialCodec.hpp>
#include <lux/engine/material/detail/BuiltinMaterialNodes.hpp>

#include <algorithm>
#include <cmath>

namespace lux::material
{
    namespace
    {
        using ShaderType = shadergen::EValueType;
        using PinList = std::vector<MaterialPinDeclaration>;

        MaterialCompileFailure invalid(std::string message, std::uint32_t pin = ~std::uint32_t{0}) noexcept
        {
            return {EMaterialCompileError::INVALID_GRAPH, std::move(message), {}, pin};
        }

        bool validType(EValueType type) noexcept
        {
            return type >= EValueType::FLOAT && type <= EValueType::VEC4;
        }

        MaterialNodeResult<void> validateSwizzle(
            const MaterialSwizzle& value,
            bool require_available_components
        ) noexcept
        {
            const bool is_invalid_type = !validType(value.source_type) || !validType(value.out_type);
            if (is_invalid_type)
            {
                return cxx::unexpected(invalid("invalid Swizzle node value type"));
            }
            const auto source_arity = static_cast<std::size_t>(value.source_type) + 1;
            const auto output_arity = static_cast<std::size_t>(value.out_type) + 1;
            for (std::size_t index = 0; index != value.components.size(); ++index)
            {
                const bool is_used = require_available_components && index < output_arity;
                const bool is_invalid =
                    value.components[index] > 3 || (is_used && value.components[index] >= source_arity);
                if (is_invalid)
                {
                    return cxx::unexpected(invalid("invalid Swizzle component", static_cast<std::uint32_t>(index)));
                }
            }
            return {};
        }

        MaterialPinDeclaration input(std::size_t index, std::string name, EValueType type) noexcept
        {
            return {graph::PinSemanticId{index + 1}, std::move(name), graph::EPinDirection::INPUT, type};
        }

        MaterialPinDeclaration output(std::string name, EValueType type) noexcept
        {
            constexpr graph::PinSemanticId Semantic{(std::uint64_t{1} << 63) | 1};
            return {Semantic, std::move(name), graph::EPinDirection::OUTPUT, type};
        }

        PinList pins(const MaterialConstant& value)
        {
            return {output("out", value.type)};
        }

        PinList pins(const MaterialInput& value)
        {
            return {output("out", materialInputDescription(value.input)->type)};
        }

        PinList pins(const MaterialSampleTexture&)
        {
            return {input(0, "uv", EValueType::VEC2), output("rgba", EValueType::VEC4)};
        }

        PinList pins(const MaterialParameter& value)
        {
            return {output("value", value.type)};
        }

        PinList pins(const MaterialSwizzle& value)
        {
            return {input(0, "in", value.source_type), output("out", value.out_type)};
        }

        PinList pins(const MaterialConstruct& value)
        {
            constexpr const char* Names[]{"x", "y", "z", "w"};
            const auto count = static_cast<std::size_t>(value.out_type) + 1;
            PinList result;
            result.reserve(count + 1);
            for (std::size_t index = 0; index != count; ++index)
            {
                result.push_back(input(index, Names[index], EValueType::FLOAT));
            }
            result.push_back(output("out", value.out_type));
            return result;
        }

        PinList pins(const MaterialDecodeNormal&)
        {
            return {input(0, "rgb", EValueType::VEC3), output("normal", EValueType::VEC3)};
        }

        PinList pins(const MaterialTbnTransform&)
        {
            return {input(0, "normal_ts", EValueType::VEC3), output("world_normal", EValueType::VEC3)};
        }

        PinList pins(const MaterialOutputSurface&)
        {
            PinList result;
            result.reserve(std::size(kMaterialAttributes));
            for (const auto& attribute : kMaterialAttributes)
            {
                auto pin = input(result.size(), attribute.name, attribute.type);
                std::copy_n(attribute.dflt, 4, pin.default_value.begin());
                pin.input_use = EMaterialInputUse::CONNECTED_VALUE;
                result.push_back(std::move(pin));
            }
            return result;
        }

        MaterialNodeResult<void> checkInputs(
            std::span<const std::uint32_t> inputs,
            const shadergen::ShaderIR& ir,
            std::size_t count,
            EValueType type
        ) noexcept
        {
            if (inputs.size() != count)
            {
                return cxx::unexpected(invalid("builtin node input count does not match its declaration"));
            }
            for (const auto index : inputs)
            {
                const bool has_value = index < ir.values.size();
                const bool is_matching = has_value && ir.values[index].type == static_cast<ShaderType>(type);
                if (!is_matching)
                {
                    return cxx::unexpected(invalid("builtin node input does not match its declaration"));
                }
            }
            return {};
        }

        std::uint32_t append(shadergen::ShaderIRValue value, shadergen::ShaderIR& ir) noexcept
        {
            const auto index = static_cast<std::uint32_t>(ir.values.size());
            ir.values.push_back(value);
            return index;
        }

        MaterialNodeResult<std::uint32_t> appendNormal(
            shadergen::EOp op,
            std::span<const std::uint32_t> inputs,
            shadergen::ShaderIR& ir
        ) noexcept
        {
            auto checked = checkInputs(inputs, ir, 1, EValueType::VEC3);
            if (!checked)
            {
                return cxx::unexpected(std::move(checked.error()));
            }
            shadergen::ShaderIRValue value{};
            value.op = op;
            value.type = ShaderType::VEC3;
            value.operands[0] = inputs[0];
            return append(value, ir);
        }
    } // namespace

    MaterialNodeResult<void> detail::validateBuiltin(const MaterialConstant& value) noexcept
    {
        const bool is_finite =
            std::all_of(value.value.begin(), value.value.end(), [](float x) noexcept { return std::isfinite(x); });
        const bool is_invalid = !validType(value.type) || !is_finite;
        if (is_invalid)
        {
            return cxx::unexpected(invalid("invalid Constant node payload"));
        }
        return {};
    }

    MaterialNodeResult<void> detail::validateBuiltin(const MaterialInput& value) noexcept
    {
        if (!materialInputDescription(value.input))
        {
            return cxx::unexpected(invalid("invalid Material input enum"));
        }
        return {};
    }

    MaterialNodeResult<void> detail::validateBuiltin(const MaterialParameter& value) noexcept
    {
        if (!validType(value.type))
        {
            return cxx::unexpected(invalid("invalid Param node payload"));
        }
        return {};
    }

    MaterialNodeResult<void> detail::validateSwizzlePayload(const MaterialSwizzle& value) noexcept
    {
        return validateSwizzle(value, false);
    }

    MaterialNodeResult<void> detail::validateBuiltin(const MaterialSwizzle& value) noexcept
    {
        return validateSwizzle(value, true);
    }

    MaterialNodeResult<void> detail::validateBuiltin(const MaterialConstruct& value) noexcept
    {
        if (!validType(value.out_type))
        {
            return cxx::unexpected(invalid("invalid Construct node payload or arity"));
        }
        return {};
    }

    MaterialNodeResult<std::uint32_t> detail::appendBuiltin(
        const MaterialConstant& payload,
        std::span<const std::uint32_t> inputs,
        shadergen::ShaderIR& ir
    ) noexcept
    {
        auto validated = validateBuiltin(payload);
        if (!validated)
        {
            return cxx::unexpected(std::move(validated.error()));
        }
        auto checked = checkInputs(inputs, ir, 0, payload.type);
        if (!checked)
        {
            return cxx::unexpected(std::move(checked.error()));
        }
        shadergen::ShaderIRValue value{};
        value.op = shadergen::EOp::CONSTANT;
        value.type = static_cast<ShaderType>(payload.type);
        std::copy(payload.value.begin(), payload.value.end(), value.constant);
        return append(value, ir);
    }

    MaterialNodeResult<std::uint32_t> detail::appendBuiltin(
        const MaterialInput& payload,
        std::span<const std::uint32_t> inputs,
        shadergen::ShaderIR& ir
    ) noexcept
    {
        auto validated = validateBuiltin(payload);
        if (!validated)
        {
            return cxx::unexpected(std::move(validated.error()));
        }
        const auto& description = *materialInputDescription(payload.input);
        auto checked = checkInputs(inputs, ir, 0, description.type);
        if (!checked)
        {
            return cxx::unexpected(std::move(checked.error()));
        }
        // The original material vertex interpolants are UV0=3, position=0, normal=1, tangent=5.
        // VertexColor remains unspecified for the shader shell to place.
        constexpr std::array<std::int32_t, 5> Locations{3, 0, 1, 5, -1};
        static_assert(Locations.size() == std::size(kMaterialInputs));
        const auto location = Locations[static_cast<std::size_t>(payload.input)];
        auto found = std::find_if(
            ir.inputs.begin(),
            ir.inputs.end(),
            [&](const auto& slot) noexcept { return slot.name == description.name; }
        );
        std::uint32_t slot{};
        if (found == ir.inputs.end())
        {
            slot = static_cast<std::uint32_t>(ir.inputs.size());
            ir.inputs.push_back({description.name, static_cast<ShaderType>(description.type), location});
        }
        else
        {
            const bool is_conflict = found->type != static_cast<ShaderType>(description.type) ||
                                     found->location != location ||
                                     found->interpolation != shadergen::EInterpolation::SMOOTH;
            if (is_conflict)
            {
                return cxx::unexpected(invalid("material shading input conflicts with an existing declaration"));
            }
            slot = static_cast<std::uint32_t>(found - ir.inputs.begin());
        }
        shadergen::ShaderIRValue value{};
        value.op = shadergen::EOp::INPUT;
        value.type = static_cast<ShaderType>(description.type);
        value.slot = slot;
        return append(value, ir);
    }

    MaterialNodeResult<std::uint32_t> detail::appendBuiltin(
        const MaterialSampleTexture& payload,
        std::span<const std::uint32_t> inputs,
        shadergen::ShaderIR& ir
    ) noexcept
    {
        auto checked = checkInputs(inputs, ir, 1, EValueType::VEC2);
        if (!checked)
        {
            return cxx::unexpected(std::move(checked.error()));
        }
        if (payload.texture_slot >= ir.textures.size())
        {
            return cxx::unexpected(invalid("SampleTexture references an undeclared texture slot"));
        }
        shadergen::ShaderIRValue value{};
        value.op = shadergen::EOp::SAMPLE_TEXTURE;
        value.type = ShaderType::VEC4;
        value.slot = payload.texture_slot;
        value.operands[0] = inputs[0];
        return append(value, ir);
    }

    MaterialNodeResult<std::uint32_t> detail::appendBuiltin(
        const MaterialParameter& payload,
        std::span<const std::uint32_t> inputs,
        shadergen::ShaderIR& ir
    ) noexcept
    {
        auto checked = checkInputs(inputs, ir, 0, payload.type);
        if (!checked)
        {
            return cxx::unexpected(std::move(checked.error()));
        }
        const bool has_slot = payload.param_slot < ir.params.size();
        const bool is_matching =
            has_slot && ir.params[payload.param_slot].type == static_cast<ShaderType>(payload.type);
        const bool is_invalid = !validType(payload.type) || !is_matching;
        if (is_invalid)
        {
            return cxx::unexpected(invalid("invalid Param node payload"));
        }
        shadergen::ShaderIRValue value{};
        value.op = shadergen::EOp::PARAM;
        value.type = ir.params[payload.param_slot].type;
        value.slot = payload.param_slot;
        return append(value, ir);
    }

    MaterialNodeResult<std::uint32_t> detail::appendBuiltin(
        const MaterialSwizzle& payload,
        std::span<const std::uint32_t> inputs,
        shadergen::ShaderIR& ir
    ) noexcept
    {
        auto validated = validateBuiltin(payload);
        if (!validated)
        {
            return cxx::unexpected(std::move(validated.error()));
        }
        auto checked = checkInputs(inputs, ir, 1, payload.source_type);
        if (!checked)
        {
            return cxx::unexpected(std::move(checked.error()));
        }
        shadergen::ShaderIRValue value{};
        value.op = shadergen::EOp::SWIZZLE;
        value.type = static_cast<ShaderType>(payload.out_type);
        value.operands[0] = inputs[0];
        std::copy(payload.components.begin(), payload.components.end(), value.swizzle);
        return append(value, ir);
    }

    MaterialNodeResult<std::uint32_t> detail::appendBuiltin(
        const MaterialConstruct& payload,
        std::span<const std::uint32_t> inputs,
        shadergen::ShaderIR& ir
    ) noexcept
    {
        auto validated = validateBuiltin(payload);
        if (!validated)
        {
            return cxx::unexpected(std::move(validated.error()));
        }
        auto checked = checkInputs(inputs, ir, static_cast<std::size_t>(payload.out_type) + 1, EValueType::FLOAT);
        if (!checked)
        {
            return cxx::unexpected(std::move(checked.error()));
        }
        shadergen::ShaderIRValue value{};
        value.op = shadergen::EOp::CONSTRUCT;
        value.type = static_cast<ShaderType>(payload.out_type);
        std::copy(inputs.begin(), inputs.end(), value.operands);
        return append(value, ir);
    }

    MaterialNodeResult<std::uint32_t> detail::appendBuiltin(
        const MaterialDecodeNormal&,
        std::span<const std::uint32_t> inputs,
        shadergen::ShaderIR& ir
    ) noexcept
    {
        return appendNormal(shadergen::EOp::DECODE_NORMAL, inputs, ir);
    }

    MaterialNodeResult<std::uint32_t> detail::appendBuiltin(
        const MaterialTbnTransform&,
        std::span<const std::uint32_t> inputs,
        shadergen::ShaderIR& ir
    ) noexcept
    {
        return appendNormal(shadergen::EOp::TBN_NORMAL, inputs, ir);
    }

    MaterialNodeResult<void> detail::appendSurface(
        std::span<const std::uint32_t> inputs,
        shadergen::ShaderIR& ir
    ) noexcept
    {
        const bool is_invalid_output = inputs.size() != std::size(kMaterialAttributes) || !ir.outputs.empty();
        if (is_invalid_output)
        {
            return cxx::unexpected(invalid("invalid OutputSurface input count or duplicate surface"));
        }
        for (std::size_t index = 0; index != inputs.size(); ++index)
        {
            if (inputs[index] == shadergen::kNoValue)
            {
                continue;
            }
            const bool has_value = inputs[index] < ir.values.size();
            const bool is_matching =
                has_value && ir.values[inputs[index]].type == static_cast<ShaderType>(kMaterialAttributes[index].type);
            if (!is_matching)
            {
                return cxx::unexpected(invalid("invalid OutputSurface input value", static_cast<std::uint32_t>(index)));
            }
        }
        ir.outputs.reserve(inputs.size());
        for (std::size_t index = 0; index != inputs.size(); ++index)
        {
            const auto& attribute = kMaterialAttributes[index];
            shadergen::Output value;
            value.name = attribute.name;
            value.value_id = inputs[index];
            value.type = static_cast<ShaderType>(attribute.type);
            std::copy_n(attribute.dflt, 4, value.dflt);
            ir.outputs.push_back(std::move(value));
        }
        return {};
    }

    namespace
    {
        template <class T> MaterialNodeResult<std::unique_ptr<T>> clone(const T& value) noexcept
        {
            return std::make_unique<T>(value);
        }

        template <class T> MaterialNodeRegistration registration() noexcept
        {
            MaterialNodeRegistration result;
            std::string name{T::TypeName};
            result.identity = {graph::nodeTypeId(name), std::move(name), 1};
            result.payload_type = cxx::typeToken<T>();
            detail::installBuiltinCodec<T, &clone<T>>(result);
            if constexpr (std::is_same_v<T, MaterialOutputSurface>)
            {
                result.role = EMaterialNodeRole::SURFACE;
            }
            if constexpr (std::is_same_v<T, MaterialSampleTexture> || std::is_same_v<T, MaterialParameter>)
            {
                result.validate_bindings = [](const MaterialNodePayload& payload,
                                              const shadergen::ShaderIR& resources) noexcept -> MaterialNodeResult<void>
                {
                    const auto& value = *payload.get<T>();
                    if constexpr (std::is_same_v<T, MaterialSampleTexture>)
                    {
                        if (value.texture_slot >= resources.textures.size())
                        {
                            return cxx::unexpected(invalid("SampleTexture references an undeclared texture slot"));
                        }
                    }
                    else
                    {
                        const bool has_slot = value.param_slot < resources.params.size();
                        const bool is_type_mismatch =
                            has_slot && resources.params[value.param_slot].type != static_cast<ShaderType>(value.type);
                        const bool is_invalid_parameter = !has_slot || is_type_mismatch;
                        if (is_invalid_parameter)
                        {
                            return cxx::unexpected(invalid("invalid Param node payload"));
                        }
                    }
                    return {};
                };
            }
            result.create = [](const object::CodeLease& code) noexcept
            { return MaterialNodePayload::make<T, &clone<T>>(code); };
            result.validate = [](const MaterialNodePayload& payload) noexcept -> MaterialNodeResult<void>
            {
                if constexpr (requires(const T& value) { detail::validateBuiltin(value); })
                {
                    return detail::validateBuiltin(*payload.get<T>());
                }
                else
                {
                    // These payloads have no intrinsic invalid state; texture-slot admission needs IR resources.
                    static_assert(std::is_same_v<T, MaterialSampleTexture> || std::is_same_v<T, MaterialDecodeNormal> || std::is_same_v<T, MaterialTbnTransform> || std::is_same_v<T, MaterialOutputSurface>);
                    return {};
                }
            };
            using PinResult = MaterialNodeRegistration::PinResult;
            result.describe_pins = [](const MaterialNodePayload& payload) noexcept -> PinResult
            {
                const auto& value = *payload.get<T>();
                MaterialNodeResult<void> validated;
                if constexpr (std::is_same_v<T, MaterialSwizzle>)
                {
                    validated = detail::validateSwizzlePayload(value);
                }
                else if constexpr (requires { detail::validateBuiltin(value); })
                {
                    validated = detail::validateBuiltin(value);
                }
                if (!validated)
                {
                    return cxx::unexpected(std::move(validated.error()));
                }
                return pins(value);
            };
            result.compile = [](const MaterialNodePayload& payload,
                                std::span<const std::uint32_t> inputs,
                                shadergen::ShaderIR& ir) noexcept -> MaterialNodeRegistration::ShaderResult
            {
                if constexpr (std::is_same_v<T, MaterialOutputSurface>)
                {
                    auto output = detail::appendSurface(inputs, ir);
                    if (!output)
                    {
                        return cxx::unexpected(std::move(output.error()));
                    }
                    return std::vector<std::uint32_t>{};
                }
                else
                {
                    auto output = detail::appendBuiltin(*payload.get<T>(), inputs, ir);
                    if (!output)
                    {
                        return cxx::unexpected(std::move(output.error()));
                    }
                    return std::vector<std::uint32_t>{*output};
                }
            };
            return result;
        }
    } // namespace

    std::array<MaterialNodeRegistration, 10> materialBuiltinRegistrations() noexcept
    {
        return {
            registration<MaterialConstant>(),
            registration<MaterialInput>(),
            registration<MaterialSampleTexture>(),
            registration<MaterialParameter>(),
            materialMathRegistration(),
            registration<MaterialSwizzle>(),
            registration<MaterialConstruct>(),
            registration<MaterialDecodeNormal>(),
            registration<MaterialTbnTransform>(),
            registration<MaterialOutputSurface>()
        };
    }
} // namespace lux::material
