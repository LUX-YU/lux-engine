#include <lux/engine/material/detail/BuiltinMaterialCodec.hpp>
#include <lux/engine/material/detail/BuiltinMaterialNodes.hpp>

#include <algorithm>
#include <cmath>
#include <lux/engine/material/detail/SlangText.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>

namespace lux::material
{
    namespace
    {
        using PinList = std::vector<MaterialPinDeclaration>;

        MaterialCompileFailure invalid(std::string message, std::uint32_t pin = ~std::uint32_t{0}) noexcept
        {
            return {EMaterialCompileError::INVALID_GRAPH, std::move(message), {}, pin};
        }

        bool validType(EValueType type) noexcept
        {
            return type >= EValueType::FLOAT && type <= EValueType::VEC4;
        }

        MaterialNodeResult<void>
        validateSwizzle(const MaterialSwizzle& value, bool require_available_components) noexcept
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

    namespace
    {
        std::string expression(const MaterialConstant& value, std::span<const std::string_view>)
        {
            return detail::slangLiteral(value.type, value.value.data());
        }

        std::string expression(const MaterialInput& value, std::span<const std::string_view>)
        {
            return "input.v" + std::to_string(detail::kMaterialInputLocations[static_cast<unsigned>(value.input)]);
        }

        std::string expression(const MaterialSampleTexture& value, std::span<const std::string_view> inputs)
        {
            return "luxSampleTexture(record.tex[" + std::to_string(value.texture_slot) + "]," + std::string(inputs[0]) +
                   ")";
        }

        std::string expression(const MaterialParameter& value, std::span<const std::string_view>)
        {
            return "record.params[" + std::to_string(value.param_slot) + "]." +
                   std::string("xyzw", unsigned(value.type) + 1);
        }

        std::string expression(const MaterialSwizzle& value, std::span<const std::string_view> inputs)
        {
            std::string result = "(" + std::string(inputs[0]) + ").";
            for (unsigned i = 0; i <= unsigned(value.out_type); ++i)
            {
                result += "xyzw"[value.components[i]];
            }
            return result;
        }

        std::string expression(const MaterialConstruct& value, std::span<const std::string_view> inputs)
        {
            std::string result = detail::slangType(value.out_type) + "(";
            for (auto input : inputs)
            {
                if (result.back() != '(')
                {
                    result += ",";
                }
                result += input;
            }
            return result + ")";
        }

        std::string expression(const MaterialDecodeNormal&, std::span<const std::string_view> inputs)
        {
            return "normalize((" + std::string(inputs[0]) + ")*2.0-1.0)";
        }

        std::string expression(const MaterialTbnTransform&, std::span<const std::string_view> inputs)
        {
            const auto normal = "(" + std::string(inputs[0]) + ")";
            return "normalize(input.v5.xyz*rsqrt(max(dot(input.v5.xyz,input.v5.xyz),1e-20))*" + normal +
                   ".x+input.v6*rsqrt(max(dot(input.v6,input.v6),1e-20))*" + normal + ".y+normalize(input.v1)*" +
                   normal + ".z)";
        }
    } // namespace

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
            result.payload_type = cxx::make_basic_type_info<T>();
            detail::installBuiltinCodec<T, &clone<T>>(result);
            if constexpr (std::is_same_v<T, MaterialOutputSurface>)
            {
                result.role = EMaterialNodeRole::SURFACE;
            }
            if constexpr (std::is_same_v<T, MaterialSampleTexture> || std::is_same_v<T, MaterialParameter>)
            {
                result.validate_bindings = [](const MaterialNodePayload& payload,
                                              const MaterialGraph& resources) noexcept -> MaterialNodeResult<void>
                {
                    const auto& value = *payload.get<T>();
                    if constexpr (std::is_same_v<T, MaterialSampleTexture>)
                    {
                        if (value.texture_slot >= resources.texture_slots.size())
                        {
                            return cxx::unexpected(invalid("SampleTexture references an undeclared texture slot"));
                        }
                    }
                    else
                    {
                        const bool has_slot = value.param_slot < resources.param_slots.size();
                        const bool is_type_mismatch =
                            has_slot && resources.param_slots[value.param_slot].type != value.type;
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
                    // These payloads have no intrinsic invalid state; texture-slot admission needs graph declarations.
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
            result.emit_slang = [](const MaterialNodePayload& payload,
                                   std::span<const std::string_view> inputs,
                                   std::span<const std::string_view> outputs,
                                   std::string& destination) noexcept -> MaterialNodeResult<void>
            {
                if constexpr (std::is_same_v<T, MaterialOutputSurface>)
                {
                    for (std::size_t index = 0; index < inputs.size(); ++index)
                    {
                        if (!inputs[index].empty())
                        {
                            destination += "lux_" + std::string(kMaterialAttributes[index].name) + " = " +
                                           std::string(inputs[index]) + ";\n";
                        }
                    }
                }
                else
                {
                    destination += std::string(outputs[0]) + " = " + expression(*payload.get<T>(), inputs) + ";\n";
                }
                return {};
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
