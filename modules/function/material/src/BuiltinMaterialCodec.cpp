#include <lux/engine/material/detail/BuiltinMaterialCodec.hpp>
#include <lux/engine/material/detail/MaterialToml.hpp>

#include <locale>
#include <sstream>
#include <type_traits>

namespace lux::material::detail
{
    namespace
    {
        template <class T> toml::table encode(const T& value)
        {
            if constexpr (std::is_same_v<T, MaterialConstant>)
            {
                return toml::table{{"type", static_cast<std::int64_t>(value.type)}, {"value", floats(value.value)}};
            }
            else if constexpr (std::is_same_v<T, MaterialInput>)
            {
                return toml::table{{"input", static_cast<std::int64_t>(value.input)}};
            }
            else if constexpr (std::is_same_v<T, MaterialSampleTexture>)
            {
                return toml::table{{"slot", value.texture_slot}};
            }
            else if constexpr (std::is_same_v<T, MaterialParameter>)
            {
                return toml::table{{"type", static_cast<std::int64_t>(value.type)}, {"slot", value.param_slot}};
            }
            else if constexpr (std::is_same_v<T, MaterialMath>)
            {
                return toml::table{
                    {"type", static_cast<std::int64_t>(value.operand_type)},
                    {"operation", static_cast<std::int64_t>(value.op)}
                };
            }
            else if constexpr (std::is_same_v<T, MaterialSwizzle>)
            {
                toml::array components;
                for (const auto component : value.components)
                {
                    components.push_back(component);
                }
                return toml::table{
                    {"source_type", static_cast<std::int64_t>(value.source_type)},
                    {"type", static_cast<std::int64_t>(value.out_type)},
                    {"components", std::move(components)}
                };
            }
            else if constexpr (std::is_same_v<T, MaterialConstruct>)
            {
                return toml::table{{"type", static_cast<std::int64_t>(value.out_type)}};
            }
            else
            {
                static_assert(std::is_empty_v<T>);
                return toml::table{};
            }
        }

        template <class T> bool decode(const toml::table& data, T& value) noexcept
        {
            const auto type = integer(data["type"], 3);
            const auto slot = integer(data["slot"], UINT32_MAX);
            if constexpr (std::is_same_v<T, MaterialConstant>)
            {
                const bool has_fields = fields(data, {"type", "value"}) && type;
                if (!has_fields || !readFloats(data["value"], value.value))
                {
                    return false;
                }
                value.type = static_cast<EValueType>(*type);
            }
            else if constexpr (std::is_same_v<T, MaterialInput>)
            {
                const auto input = integer(data["input"], static_cast<unsigned>(EMaterialInput::COUNT) - 1);
                const bool has_fields = fields(data, {"input"}) && input;
                if (!has_fields)
                {
                    return false;
                }
                value.input = static_cast<EMaterialInput>(*input);
            }
            else if constexpr (std::is_same_v<T, MaterialSampleTexture>)
            {
                const bool has_fields = fields(data, {"slot"}) && slot;
                if (!has_fields)
                {
                    return false;
                }
                value.texture_slot = *slot;
            }
            else if constexpr (std::is_same_v<T, MaterialParameter>)
            {
                const bool has_fields = fields(data, {"type", "slot"}) && type && slot;
                if (!has_fields)
                {
                    return false;
                }
                value.type = static_cast<EValueType>(*type);
                value.param_slot = *slot;
            }
            else if constexpr (std::is_same_v<T, MaterialMath>)
            {
                const auto operation = integer(data["operation"], static_cast<unsigned>(EMathOp::LENGTH));
                const bool has_fields = fields(data, {"type", "operation"}) && type && operation;
                if (!has_fields)
                {
                    return false;
                }
                value.operand_type = static_cast<EValueType>(*type);
                value.op = static_cast<EMathOp>(*operation);
            }
            else if constexpr (std::is_same_v<T, MaterialSwizzle>)
            {
                const auto source_type = integer(data["source_type"], 3);
                const auto* components = data["components"].as_array();
                const bool has_fields = fields(data, {"type", "source_type", "components"}) && type && source_type;
                const bool has_components = components && components->size() == 4;
                const bool is_valid = has_fields && has_components;
                if (!is_valid)
                {
                    return false;
                }
                value.source_type = static_cast<EValueType>(*source_type);
                value.out_type = static_cast<EValueType>(*type);
                for (std::size_t index = 0; index != 4; ++index)
                {
                    const auto component = integer(toml::node_view<const toml::node>{&(*components)[index]}, 3);
                    if (!component)
                    {
                        return false;
                    }
                    value.components[index] = static_cast<std::uint8_t>(*component);
                }
            }
            else if constexpr (std::is_same_v<T, MaterialConstruct>)
            {
                const bool has_fields = fields(data, {"type"}) && type;
                if (!has_fields)
                {
                    return false;
                }
                value.out_type = static_cast<EValueType>(*type);
            }
            else
            {
                static_assert(std::is_empty_v<T>);
                return data.empty();
            }
            return true;
        }
    } // namespace

    template <class T> MaterialNodeResult<std::string> encodeBuiltinPayload(const T& value) noexcept
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << toml::toml_formatter{encode(value)};
        return std::move(stream).str();
    }

    template <class T> MaterialNodeResult<T> decodeBuiltinPayload(std::string_view bytes) noexcept
    {
        auto parsed = toml::parse(bytes);
        T value;
        if (!parsed || !decode(parsed.table(), value))
        {
            return cxx::unexpected(
                MaterialCompileFailure{EMaterialCompileError::INVALID_GRAPH, "invalid material node source payload"}
            );
        }
        return value;
    }

#define LUX_INSTANTIATE_MATERIAL_CODEC(Type)                                                                           \
    template MaterialNodeResult<std::string> encodeBuiltinPayload(const Type&) noexcept;                               \
    template MaterialNodeResult<Type> decodeBuiltinPayload(std::string_view) noexcept;

    LUX_INSTANTIATE_MATERIAL_CODEC(MaterialConstant)
    LUX_INSTANTIATE_MATERIAL_CODEC(MaterialInput)
    LUX_INSTANTIATE_MATERIAL_CODEC(MaterialSampleTexture)
    LUX_INSTANTIATE_MATERIAL_CODEC(MaterialParameter)
    LUX_INSTANTIATE_MATERIAL_CODEC(MaterialMath)
    LUX_INSTANTIATE_MATERIAL_CODEC(MaterialSwizzle)
    LUX_INSTANTIATE_MATERIAL_CODEC(MaterialConstruct)
    LUX_INSTANTIATE_MATERIAL_CODEC(MaterialDecodeNormal)
    LUX_INSTANTIATE_MATERIAL_CODEC(MaterialTbnTransform)
    LUX_INSTANTIATE_MATERIAL_CODEC(MaterialOutputSurface)

#undef LUX_INSTANTIATE_MATERIAL_CODEC
} // namespace lux::material::detail
