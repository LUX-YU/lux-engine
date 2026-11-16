#include <lux/engine/material/compiler/ShaderSources.hpp>
#include <lux/engine/material/compiler/SlangBackend.hpp>

#include <algorithm>
#include <charconv>
#include <map>

namespace lux::material::detail
{
    namespace
    {
        using shadergen::EOp;
        using shadergen::EValueType;

        std::string number(float value)
        {
            char buffer[64];
            const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
            std::string result(buffer, converted.ptr);
            if (result.find_first_of(".eE") == std::string::npos)
            {
                result += ".0";
            }
            return result;
        }

        std::string typeName(EValueType type)
        {
            return type == EValueType::FLOAT ? "float" : "float" + std::to_string(static_cast<unsigned>(type) + 1);
        }

        std::string literal(EValueType type, const float* values)
        {
            std::string result = typeName(type) + "(";
            for (unsigned component = 0; component <= static_cast<unsigned>(type); ++component)
            {
                if (component)
                {
                    result += ",";
                }
                result += number(values[component]);
            }
            return result + ")";
        }
    } // namespace

    cxx::expected<std::string, MaterialDeriveFailure>
    emitSlang(const MaterialIR& material, rdesc::EMaterialPurpose purpose)
    {
        const auto fail = [](std::string text)
        { return cxx::unexpected(MaterialDeriveFailure{EMaterialDeriveError::UNSUPPORTED_SOURCE, std::move(text)}); };
        const auto& ir = material.shader;
        const bool is_shadow = purpose == rdesc::EMaterialPurpose::SHADOW;
        const bool is_gbuffer = purpose == rdesc::EMaterialPurpose::GBUFFER;
        const bool is_unlit = material.shading_model == rdesc::ELightingTechnique::UNLIT;
        const bool is_pbr = material.shading_model == rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
        const bool is_unsupported_model =
            (!is_shadow && !is_gbuffer && !is_unlit) || (is_gbuffer && !is_unlit && !is_pbr);
        if (is_unsupported_model || material.alpha_mode == rdesc::EAlphaMode::BLEND)
        {
            return fail("material model/alpha mode is not supported for this purpose");
        }

        std::vector<std::uint8_t> selected(ir.values.size());
        const auto find = [&](std::string_view name)
        { return std::ranges::find(ir.outputs, name, &shadergen::Output::name); };
        const auto select = [&](std::string_view name, EValueType type)
        {
            const auto output = find(name);
            if (output == ir.outputs.end() || output->type != type)
            {
                return false;
            }
            if (output->value_id != shadergen::kNoValue)
            {
                selected[output->value_id] = 1;
            }
            return true;
        };
        if (!is_shadow && (!select("base_color", EValueType::VEC3) || !select("emissive", EValueType::VEC3)))
        {
            return fail("material is missing color outputs");
        }
        if (is_gbuffer && !is_unlit &&
            (!select("metallic", EValueType::FLOAT) || !select("roughness", EValueType::FLOAT) ||
             !select("normal_world", EValueType::VEC3) || !select("ao", EValueType::FLOAT)))
        {
            return fail("material is missing GBuffer outputs");
        }
        if ((!is_shadow && !is_gbuffer) || material.alpha_mode == rdesc::EAlphaMode::MASK)
        {
            if (!select("opacity", EValueType::FLOAT))
            {
                return fail("material is missing opacity");
            }
        }
        for (std::size_t index = selected.size(); index != 0; --index)
        {
            if (!selected[index - 1])
            {
                continue;
            }
            for (auto operand : ir.values[index - 1].operands)
            {
                if (operand != shadergen::kNoValue)
                {
                    selected[operand] = 1;
                }
            }
        }

        struct Input final
        {
            std::string type;
            bool flat;
        };

        std::map<std::int32_t, Input> inputs;
        const auto input = [&](std::int32_t location, std::string type, bool flat = false)
        {
            if (location < 0)
            {
                return false;
            }
            const auto [position, inserted] = inputs.emplace(location, Input{type, flat});
            return inserted || (position->second.type == type && position->second.flat == flat);
        };
        const auto valueName = [](std::uint32_t id) { return "v" + std::to_string(id); };
        const auto output = [&](std::string_view name)
        {
            const auto value = find(name);
            return value->value_id == shadergen::kNoValue ? literal(value->type, value->dflt)
                                                          : valueName(value->value_id);
        };
        bool uses_material = false, uses_textures = false;
        std::string body;
        for (std::size_t index = 0; index < ir.values.size(); ++index)
        {
            if (!selected[index])
            {
                continue;
            }
            const auto& value = ir.values[index];
            const auto argument = [&](unsigned slot) { return valueName(value.operands[slot]); };
            const auto call = [&](std::string_view name, unsigned count)
            {
                std::string expression(name);
                expression += "(";
                for (unsigned slot = 0; slot < count; ++slot)
                {
                    if (slot)
                    {
                        expression += ",";
                    }
                    expression += argument(slot);
                }
                return expression + ")";
            };
            std::string expression;
            switch (value.op)
            {
            case EOp::CONSTANT:
                expression = literal(value.type, value.constant);
                break;
            case EOp::INPUT:
            {
                const auto& source = ir.inputs[value.slot];
                if (!input(
                        source.location,
                        typeName(source.type),
                        source.interpolation == shadergen::EInterpolation::FLAT
                    ))
                {
                    return fail("material input locations conflict or are unspecified");
                }
                expression = "input.v" + std::to_string(source.location);
                break;
            }
            case EOp::PARAM:
                uses_material = true;
                expression = "record.params[" + std::to_string(value.slot) + "]." +
                             std::string("xyzw", static_cast<unsigned>(value.type) + 1);
                break;
            case EOp::SAMPLE_TEXTURE:
                uses_material = uses_textures = true;
                expression = "luxSampleTexture(record.tex[" + std::to_string(value.slot) + "]," + argument(0) + ")";
                break;
            case EOp::MUL:
                expression = argument(0) + "*" + argument(1);
                break;
            case EOp::ADD:
                expression = argument(0) + "+" + argument(1);
                break;
            case EOp::SUB:
                expression = argument(0) + "-" + argument(1);
                break;
            case EOp::DIV:
                expression = argument(0) + "/" + argument(1);
                break;
            case EOp::MOD:
                expression = argument(0) + "-" + argument(1) + "*floor(" + argument(0) + "/" + argument(1) + ")";
                break;
            case EOp::DOT:
                expression = call("dot", 2);
                break;
            case EOp::MIN:
                expression = call("min", 2);
                break;
            case EOp::MAX:
                expression = call("max", 2);
                break;
            case EOp::POW:
                expression = call("pow", 2);
                break;
            case EOp::STEP:
                expression = call("step", 2);
                break;
            case EOp::CROSS:
                expression = call("cross", 2);
                break;
            case EOp::REFLECT:
                expression = call("reflect", 2);
                break;
            case EOp::LERP:
                expression = call("lerp", 3);
                break;
            case EOp::SATURATE:
                expression = call("saturate", 1);
                break;
            case EOp::ONE_MINUS:
                expression = "1.0-" + argument(0);
                break;
            case EOp::ABS:
                expression = call("abs", 1);
                break;
            case EOp::SQRT:
                expression = call("sqrt", 1);
                break;
            case EOp::FLOOR:
                expression = call("floor", 1);
                break;
            case EOp::FRACT:
                expression = call("frac", 1);
                break;
            case EOp::SIN:
                expression = call("sin", 1);
                break;
            case EOp::COS:
                expression = call("cos", 1);
                break;
            case EOp::NORMALIZE:
                expression = call("normalize", 1);
                break;
            case EOp::LENGTH:
                expression = call("length", 1);
                break;
            case EOp::DECODE_NORMAL:
                expression = "normalize(" + argument(0) + "*2.0-1.0)";
                break;
            case EOp::CONSTRUCT:
                expression = call(typeName(value.type), static_cast<unsigned>(value.type) + 1);
                break;
            case EOp::SWIZZLE:
                expression = argument(0) + ".";
                for (unsigned component = 0; component <= static_cast<unsigned>(value.type); ++component)
                {
                    expression += "xyzw"[value.swizzle[component]];
                }
                break;
            case EOp::TBN_NORMAL:
                if (!input(1, "float3") || !input(5, "float4") || !input(6, "float3"))
                {
                    return fail("TBN input locations conflict");
                }
                expression = "normalize(input.v5.xyz*rsqrt(max(dot(input.v5.xyz,input.v5.xyz),1e-20))*" + argument(0) +
                             ".x+input.v6*rsqrt(max(dot(input.v6,input.v6),1e-20))*" + argument(0) +
                             ".y+normalize(input.v1)*" + argument(0) + ".z)";
                break;
            case EOp::RAW_EXPR:
            {
                const auto& block = ir.raw_blocks[value.slot];
                if (block.language != "slang" && block.language != "hlsl")
                {
                    return fail("RAW_EXPR language does not match the Slang backend");
                }
                for (std::size_t character = 0; character < block.code.size(); ++character)
                {
                    if (block.code[character] == '$')
                    {
                        expression += argument(block.code[++character] - '0');
                    }
                    else
                    {
                        expression += block.code[character];
                    }
                }
                break;
            }
            }
            body += "    " + typeName(value.type) + " " + valueName(static_cast<std::uint32_t>(index)) + " = " +
                    expression + ";\n";
        }
        if (uses_material && !input(4, "uint2", true))
        {
            return fail("material reference location conflicts");
        }
        std::string normal = "float3(0,0,1)";
        if (is_gbuffer && !is_unlit)
        {
            if (find("normal_world")->value_id == shadergen::kNoValue)
            {
                if (!input(1, "float3"))
                {
                    return fail("world normal input conflicts");
                }
                normal = "normalize(input.v1)";
            }
            else
            {
                normal = "normalize(" + output("normal_world") + ")";
            }
        }
        std::string source = "// Lux material IR schema 1; target layout is reflected independently.\n";
        source += "#define vec2 float2\n#define vec3 float3\n#define vec4 float4\n";
        source += kMaterialTypesSource;
        if (is_gbuffer)
        {
            source += kGBufferSource;
        }
        source += "struct Inputs {\n";
        for (const auto& [location, value] : inputs)
        {
            source += "[[vk::location(" + std::to_string(location) + ")]] " + (value.flat ? "nointerpolation " : "") +
                      value.type + " v" + std::to_string(location) + " : TEXCOORD" + std::to_string(location) + ";\n";
        }
        source += "bool front : SV_IsFrontFace;\n};\n";
        if (uses_material)
        {
            source += "struct GraphMaterialRecord { float4 params[" +
                      std::to_string(std::max<std::size_t>(1, ir.params.size())) + "]; TextureRefGPU tex[" +
                      std::to_string(std::max<std::size_t>(1, ir.textures.size())) + "]; };\n";
            source += "[[vk::binding(4,4)]] StructuredBuffer<GraphMaterialRecord> uMats[] : register(t4,space4);\n";
            body = "    GraphMaterialRecord record = uMats[NonUniformResourceIndex(input.v4.x)][input.v4.y];\n" + body;
        }
        if (uses_textures)
        {
            source += "[[vk::binding(0,2)]] Texture2D<float4> uTex[] : register(t0,space2);\n"
                      "[[vk::binding(2,2)]] SamplerState uTexSamplers[] : register(s2,space2);\n"
                      "float4 luxSampleTexture(TextureRefGPU reference, float2 uv) {\n"
                      "float2 dx=ddx(uv), dy=ddy(uv);\n"
                      "if ((reference.flags & " +
                      std::to_string(rdesc::kTextureDefaultWhite) +
                      "u)!=0u) return float4(1.0);\n"
                      "if (reference.representation_index!=0u) return float4(1,0,1,1);\n"
                      "return uTex[NonUniformResourceIndex(reference.resource_index)].SampleGrad("
                      "uTexSamplers[NonUniformResourceIndex(reference.sampler_index)],uv,dx,dy);\n}\n";
        }
        if (is_gbuffer)
        {
            source += "struct Outputs { float4 albedo : SV_Target0; float4 normal : SV_Target1; float4 emissive : "
                      "SV_Target2; };\n";
        }
        source += "[shader(\"fragment\")]\n";
        source += is_shadow
                      ? "void main(Inputs input) {\n"
                      : (is_gbuffer ? "Outputs main(Inputs input) {\n" : "float4 main(Inputs input) : SV_Target0 {\n");
        source += body;
        if (material.alpha_mode == rdesc::EAlphaMode::MASK)
        {
            source += "if (" + output("opacity") + " < " + number(material.alpha_cutoff) + ") discard;\n";
        }
        if (is_gbuffer)
        {
            source += "Outputs result; float3 normal=" + normal + ";\n";
            if (!is_unlit)
            {
                source += "if (!input.front) normal=-normal;\n";
            }
            source +=
                "result.albedo=float4(" + output("base_color") + "," + (is_unlit ? "0.0" : output("metallic")) + ");\n";
            source += "result.normal=packGNormalSM(normal," + std::string(is_unlit ? "LUX_SM_UNLIT" : "LUX_SM_PBR") +
                      "," + (is_unlit ? "1.0" : output("roughness")) + ");\n";
            source += "result.emissive=float4(" + output("emissive") + "," + (is_unlit ? "1.0" : output("ao")) +
                      ");\nreturn result;\n";
        }
        else if (!is_shadow)
        {
            source +=
                "return float4(" + output("base_color") + "+" + output("emissive") + "," + output("opacity") + ");\n";
        }
        return source + "}\n";
    }
} // namespace lux::material::detail
