// =============================================================================
//  ShaderEmitter.cpp  --  ShaderIR -> GLSL fragment source (pure, no shaderc/no device)
// -----------------------------------------------------------------------------
//  Walks the SSA values in topological order, emitting one GLSL local per value
//  (giving automatic CSE), then emits the shell epilogue based on pass/shading_model.
//  Emits surface expressions for each requested material purpose:
//    - it consumes the generic ShaderIR: Input references come from
//      self-contained InputSlot entries (name/type/location), and outputs are
//      looked up by name (falling back to Output.dflt when value_id==kNoValue);
//    - the shell #includes the real single-source-of-truth file
//      (gbuffer_encode.glsl) instead of hand-copying it inline (ShaderCompiler
//      wires up an IncluderInterface for this).
//
//  Forward Unlit, GBuffer and Shadow coverage share one canonical parameter layout.
//  Normal decode and tangent-to-world transformation are explicit graph operations.
//  RawExpr and visibility-buffer output remain unsupported.
// =============================================================================

#include <lux/engine/material/compiler/Backend.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <lux/engine/description/MaterialLayout.hpp>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace rdesc = ::lux::rdesc;

namespace lux::shadergen::glsl
{
    namespace
    {
        using EValueType = ::lux::shadergen::EValueType;

        const char* glslType(EValueType t)
        {
            switch (t)
            {
            case EValueType::FLOAT:
                return "float";
            case EValueType::VEC2:
                return "vec2";
            case EValueType::VEC3:
                return "vec3";
            case EValueType::VEC4:
                return "vec4";
            }
            return "float";
        }

        int arity(EValueType t)
        {
            switch (t)
            {
            case EValueType::FLOAT:
                return 1;
            case EValueType::VEC2:
                return 2;
            case EValueType::VEC3:
                return 3;
            case EValueType::VEC4:
                return 4;
            }
            return 1;
        }

        // Shortest-round-trip float literal, always with a decimal point (to
        // guarantee it reads as a float, not an int).
        std::string fmtF(float f)
        {
            char buf[64];
            const auto res = std::to_chars(buf, buf + sizeof(buf), f);
            std::string s(buf, res.ptr);
            if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
                s.find('E') == std::string::npos && s.find("inf") == std::string::npos &&
                s.find("nan") == std::string::npos)
            {
                s += ".0";
            }
            return s;
        }

        std::string literal(EValueType t, const float* c)
        {
            const int n = arity(t);
            if (n == 1)
            {
                return fmtF(c[0]);
            }
            std::string s = glslType(t);
            s += "(";
            for (int i = 0; i < n; ++i)
            {
                if (i)
                {
                    s += ", ";
                }
                s += fmtF(c[i]);
            }
            s += ")";
            return s;
        }

        // Material stage input names match the mesh feature's vertex outputs.
        std::string interpName(const std::string& glsl_name)
        {
            if (glsl_name == "world_position")
            {
                return "vWorldPos";
            }
            if (glsl_name == "world_normal")
            {
                return "vWorldNormal";
            }
            if (glsl_name == "uv0")
            {
                return "vUV";
            }
            if (glsl_name == "world_tangent")
            {
                return "vWorldTangent";
            }
            return glsl_name;
        }

        struct Emitter
        {
            const ShaderIR& ir;
            std::string* error;
            bool ok = true;
            bool tex_mode = false; // Set when there's a SampleTexture: needs set-2 uTex[] + nonuniform
            bool need_mat = false; // Set when there's tex/param: needs the cooked material record and vMatIndex
            std::ostringstream body;
            // Shading inputs actually used: location -> (glslType, name). Using a
            // map guarantees declared_in iterates in ascending location order.
            std::map<int, std::pair<std::string, std::string>> declared_in;

            Emitter(const ShaderIR& ir_, std::string* e_) : ir(ir_), error(e_) {}

            bool fail(std::string m)
            {
                if (error && ok)
                {
                    *error = std::move(m);
                }
                ok = false;
                return false;
            }

            static std::string vname(uint32_t i)
            {
                return "v" + std::to_string(i);
            }

            std::string operand(const ShaderIRValue& v, int k)
            {
                const uint32_t idx = v.operands[k];
                if (idx == kNoValue || idx >= ir.values.size())
                {
                    fail("shadergen: invalid ShaderIR operand index");
                    return "0.0";
                }
                return vname(idx);
            }

            void useInput(const InputSlot& s)
            {
                if (s.location < 0)
                {
                    return;
                }
                const char* type = glslType(s.type);
                declared_in[s.location] = {type, interpName(s.name)};
            }

            // The epilogue needs a world-normal fallback (PBR/GBuffer normalExpr
            // fallback / Unlit normal). Force-declare it at location 1 (matching
            // the material vertex shader); if the graph also uses
            // Input(WorldNormal), the matching location/name naturally dedupes.
            std::string ensureWorldNormal()
            {
                declared_in[1] = {"vec3", "vWorldNormal"};
                return "vWorldNormal";
            }

            bool emitValues(std::span<const std::uint8_t> selected)
            {
                // Per-material data (textures/params) is fetched from the set-4
                // Cooked material record, indexed by the flat vMatIndex
                // interpolant (matching gbuffer_pbr.frag).
                if (need_mat)
                {
                    declared_in[4] = {"flat uint", "vMatIndex"};
                    body << "    GraphMaterialRecord _mat = " << rdesc::kMaterialTableName << ".mats[vMatIndex];\n";
                }
                for (size_t i = 0; i < ir.values.size(); ++i)
                {
                    if (!selected[i])
                    {
                        continue;
                    }
                    const ShaderIRValue& v = ir.values[i];
                    std::string rhs;
                    switch (v.op)
                    {
                    case EOp::CONSTANT:
                        rhs = literal(v.type, v.constant);
                        break;
                    case EOp::INPUT:
                    {
                        if (v.slot >= ir.inputs.size())
                        {
                            fail("shadergen: Input slot out of range");
                            break;
                        }
                        const InputSlot& s = ir.inputs[v.slot];
                        useInput(s);
                        rhs = interpName(s.name);
                        break;
                    }
                    case EOp::MUL:
                        rhs = operand(v, 0) + " * " + operand(v, 1);
                        break;
                    case EOp::ADD:
                        rhs = operand(v, 0) + " + " + operand(v, 1);
                        break;
                    case EOp::SUB:
                        rhs = operand(v, 0) + " - " + operand(v, 1);
                        break;
                    case EOp::DIV:
                        rhs = operand(v, 0) + " / " + operand(v, 1);
                        break;
                    case EOp::DOT:
                        rhs = "dot(" + operand(v, 0) + ", " + operand(v, 1) + ")";
                        break;
                    case EOp::MIN:
                        rhs = "min(" + operand(v, 0) + ", " + operand(v, 1) + ")";
                        break;
                    case EOp::MAX:
                        rhs = "max(" + operand(v, 0) + ", " + operand(v, 1) + ")";
                        break;
                    case EOp::POW:
                        rhs = "pow(" + operand(v, 0) + ", " + operand(v, 1) + ")";
                        break;
                    case EOp::STEP:
                        rhs = "step(" + operand(v, 0) + ", " + operand(v, 1) + ")";
                        break;
                    case EOp::MOD:
                        rhs = "mod(" + operand(v, 0) + ", " + operand(v, 1) + ")";
                        break;
                    case EOp::CROSS:
                        rhs = "cross(" + operand(v, 0) + ", " + operand(v, 1) + ")";
                        break;
                    case EOp::REFLECT:
                        rhs = "reflect(" + operand(v, 0) + ", " + operand(v, 1) + ")";
                        break;
                    case EOp::LERP:
                        rhs = "mix(" + operand(v, 0) + ", " + operand(v, 1) + ", " + operand(v, 2) + ")";
                        break;
                    case EOp::SATURATE:
                        rhs = "clamp(" + operand(v, 0) + ", 0.0, 1.0)";
                        break;
                    case EOp::ONE_MINUS:
                        rhs = "1.0 - " + operand(v, 0);
                        break;
                    case EOp::ABS:
                        rhs = "abs(" + operand(v, 0) + ")";
                        break;
                    case EOp::SQRT:
                        rhs = "sqrt(" + operand(v, 0) + ")";
                        break;
                    case EOp::FLOOR:
                        rhs = "floor(" + operand(v, 0) + ")";
                        break;
                    case EOp::FRACT:
                        rhs = "fract(" + operand(v, 0) + ")";
                        break;
                    case EOp::SIN:
                        rhs = "sin(" + operand(v, 0) + ")";
                        break;
                    case EOp::COS:
                        rhs = "cos(" + operand(v, 0) + ")";
                        break;
                    case EOp::NORMALIZE:
                        rhs = "normalize(" + operand(v, 0) + ")";
                        break;
                    case EOp::LENGTH:
                        rhs = "length(" + operand(v, 0) + ")";
                        break;
                    case EOp::DECODE_NORMAL:
                        rhs = "normalize(" + operand(v, 0) + ".xyz * 2.0 - 1.0)";
                        break;
                    case EOp::SWIZZLE:
                    {
                        std::string sw;
                        for (int k = 0; k < arity(v.type); ++k)
                        {
                            sw += "xyzw"[v.swizzle[k] & 3u];
                        }
                        rhs = operand(v, 0) + "." + sw;
                        break;
                    }
                    case EOp::CONSTRUCT:
                    {
                        rhs = std::string(glslType(v.type)) + "(";
                        bool first = true;
                        for (int k = 0; k < 4; ++k)
                        {
                            if (v.operands[k] == kNoValue)
                            {
                                break;
                            }
                            if (!first)
                            {
                                rhs += ", ";
                            }
                            rhs += operand(v, k);
                            first = false;
                        }
                        rhs += ")";
                        break;
                    }
                    // Needs set2/set4 data binding / an additional interpolant / backend-specific text.
                    case EOp::SAMPLE_TEXTURE:
                        // TextureRefGPU is the representation-aware ABI shared
                        // with builtin material families. Keep graph sampling on
                        // the same helper so a future provider does not require a
                        // second graph-only field convention.
                        rhs = "luxSampleTexture(_mat." + std::string(rdesc::kMaterialTexturesName) + "[" +
                              std::to_string(v.slot) + "], " + operand(v, 0) + ")";
                        break;
                    case EOp::PARAM:
                    {
                        // Reads one param lane from the Graph-family SSBO
                        // (slot i -> params[i]), then swizzles it down to the
                        // declared arity.
                        const int n = arity(v.type);
                        const char* sw = (n == 1) ? "x" : (n == 2) ? "xy" : (n == 3) ? "xyz" : "xyzw";
                        rhs = "_mat." + std::string(rdesc::kMaterialParametersName) + "[" + std::to_string(v.slot) +
                              "]." + sw;
                        break;
                    }
                    case EOp::TBN_NORMAL:
                    {
                        // Tangent-space normal -> world (mat3(T,B,N) * n), using the
                        // interpolated T/B/N (mirroring the engine's
                        // calculateWorldNormal). Declares the interpolant directly:
                        // world_bitangent@6 is not an EMaterialInput, and the old
                        // emitter also declared it directly; using the world_*
                        // naming consistently lets it dedupe against the Input
                        // slots (world_normal/world_tangent) without causing a
                        // location conflict.
                        declared_in[1] = {"vec3", "vWorldNormal"};
                        declared_in[5] = {"vec4", "vWorldTangent"};
                        declared_in[6] = {"vec3", "vWorldBitangent"};
                        // Tangent-space normal -> world. The 2*rgb-1 decode is the
                        // SEPARATE upstream DecodeNormal op (EOp::DECODE_NORMAL); do NOT
                        // re-decode here or a DecodeNormal->TbnTransform chain double-decodes.
                        rhs = "normalize(mat3(normalize(vWorldTangent.xyz), normalize(vWorldBitangent), "
                              "normalize(vWorldNormal)) * " +
                              operand(v, 0) + ")";
                        break;
                    }
                    case EOp::RAW_EXPR:
                        return fail("shadergen(M1): RawExpr 暂不支持（逃生舱后续）");
                    default:
                        return fail("shadergen: unsupported ShaderIR op");
                    }
                    if (!ok)
                    {
                        return false;
                    }
                    body << "    " << glslType(v.type) << " " << vname(static_cast<uint32_t>(i)) << " = " << rhs
                         << ";\n";
                }
                return ok;
            }

            // Named output: use its local value if value_id is valid, otherwise
            // fall back to the self-contained dflt.
            std::string attr(const char* name) const
            {
                for (const auto& o : ir.outputs)
                {
                    if (o.name != name)
                    {
                        continue;
                    }
                    if (o.value_id != kNoValue && o.value_id < ir.values.size())
                    {
                        return vname(o.value_id);
                    }
                    return literal(o.type, o.dflt);
                }
                return "0.0"; // outputs always contains every material property; this should be unreachable in practice
            }

            bool hasBoundOutput(const char* name) const
            {
                for (const auto& o : ir.outputs)
                {
                    if (o.name == name)
                    {
                        return o.value_id != kNoValue && o.value_id < ir.values.size();
                    }
                }
                return false;
            }
        };
    } // namespace

    lux::cxx::expected<std::string, std::string> emitGlsl(const ShaderIR& ir, const EmitParams& parameters)
    {
        const bool shadow = parameters.purpose == rdesc::EMaterialPurpose::SHADOW;
        const bool gbuffer = parameters.purpose == rdesc::EMaterialPurpose::GBUFFER;
        const bool forward = parameters.purpose == rdesc::EMaterialPurpose::FORWARD;
        const bool unlit = parameters.shading_model == rdesc::ELightingTechnique::UNLIT;
        const bool pbr = parameters.shading_model == rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
        const bool unsupported_purpose =
            (!shadow && !gbuffer && !forward) || (forward && !unlit) || (gbuffer && !unlit && !pbr);
        if (unsupported_purpose)
        {
            return lux::cxx::unexpected(std::string("shadergen: requested purpose does not support this shading model")
            );
        }
        if (parameters.alpha_mode == rdesc::EAlphaMode::BLEND)
        {
            return lux::cxx::unexpected(std::string("shadergen: transparent material use is not supported"));
        }

        // Each use consumes only its surface outputs. Mark operands backwards in the existing
        // topological IR; do not copy the graph or change the shared material record layout.
        std::vector<std::uint8_t> selected(ir.values.size(), 0);
        bool invalid_output = false;
        const auto select = [&](std::string_view name)
        {
            const auto output = std::ranges::find(ir.outputs, name, &Output::name);
            if (output != ir.outputs.end() && output->value_id != kNoValue)
            {
                if (output->value_id >= selected.size())
                {
                    invalid_output = true;
                }
                else
                {
                    selected[output->value_id] = 1;
                }
            }
        };
        if (!shadow)
        {
            select("base_color");
            select("emissive");
        }
        if (gbuffer && !unlit)
        {
            for (auto name : {"metallic", "roughness", "normal_world", "ao"})
            {
                select(name);
            }
        }
        if (forward || parameters.alpha_mode == rdesc::EAlphaMode::MASK)
        {
            select("opacity");
        }
        if (invalid_output)
        {
            return lux::cxx::unexpected(std::string("shadergen: invalid material output value"));
        }
        for (std::size_t i = selected.size(); i != 0; --i)
        {
            if (!selected[i - 1])
            {
                continue;
            }
            for (auto operand : ir.values[i - 1].operands)
            {
                if (operand != kNoValue)
                {
                    if (operand >= i - 1)
                    {
                        return lux::cxx::unexpected(std::string("shadergen: material operand must precede its use"));
                    }
                    selected[operand] = 1;
                }
            }
        }

        std::string error;
        Emitter emitter{ir, &error};
        for (std::size_t i = 0; i < selected.size(); ++i)
        {
            if (selected[i])
            {
                emitter.tex_mode |= ir.values[i].op == EOp::SAMPLE_TEXTURE;
                emitter.need_mat |= ir.values[i].op == EOp::PARAM;
            }
        }
        emitter.need_mat |= emitter.tex_mode;
        if (!emitter.emitValues(selected))
        {
            return lux::cxx::unexpected(error.empty() ? std::string("shadergen: emitValues failed") : std::move(error));
        }

        std::string normal;
        if (gbuffer)
        {
            const bool normal_bound = !unlit && emitter.hasBoundOutput("normal_world");
            normal = unlit          ? "vec3(0.0, 0.0, 1.0)"
                     : normal_bound ? "normalize(" + emitter.attr("normal_world") + ")"
                                    : "normalize(" + emitter.ensureWorldNormal() + ")";
        }

        std::ostringstream out;
        out << "#version 450\n";
        out << "#define LUX_MATERIAL_SHADER_ABI " << rdesc::kMaterialShaderAbi << "u\n";
        out << "#define LUX_TEXTURE_DEFAULT_WHITE " << rdesc::kTextureDefaultWhite << "u\n";
        if (emitter.tex_mode)
        {
            out << "#extension GL_EXT_nonuniform_qualifier : require\n";
        }
        for (const auto& [location, input] : emitter.declared_in)
        {
            out << "layout(location = " << location << ") in " << input.first << " " << input.second << ";\n";
        }
        if (gbuffer)
        {
            out << "layout(location = 0) out vec4 gAlbedoMetallic;\n";
            out << "layout(location = 1) out vec4 gNormalRoughness;\n";
            out << "layout(location = 2) out vec4 gEmissiveAo;\n";
        }
        else if (forward)
        {
            out << "layout(location = 0) out vec4 outColor;\n";
        }
        if (emitter.need_mat)
        {
            out << "#include \"material_types.glsl\"\n";
            out << "struct GraphMaterialRecord {\n"
                << " vec4 " << rdesc::kMaterialParametersName << "[" << std::max<std::size_t>(1, ir.params.size())
                << "];\n"
                << " TextureRefGPU " << rdesc::kMaterialTexturesName << "["
                << std::max<std::size_t>(1, ir.textures.size()) << "];\n};\n";
            if (emitter.tex_mode)
            {
                out << "layout(set = 2, binding = 0) uniform texture2D uTex[];\n";
                out << "layout(set = 2, binding = 2) uniform sampler uTexSamplers[];\n";
                out << "#include \"texture_sampling.glsl\"\n";
            }
            out << "layout(set = 4, binding = 4, std430) readonly buffer MatBuf {\n"
                << "    GraphMaterialRecord mats[];\n"
                << "} " << rdesc::kMaterialTableName << ";\n";
        }
        if (gbuffer)
        {
            out << "#include \"gbuffer_encode.glsl\"\n";
        }

        out << "\nvoid main()\n{\n";
        out << emitter.body.str();
        if (parameters.alpha_mode == rdesc::EAlphaMode::MASK)
        {
            out << "    if (" << emitter.attr("opacity") << " < " << fmtF(parameters.alpha_cutoff) << ") discard;\n";
        }
        if (gbuffer)
        {
            if (unlit)
            {
                out << "    gAlbedoMetallic  = vec4(" << emitter.attr("base_color") << ", 0.0);\n";
                out << "    gNormalRoughness = packGNormalSM(" << normal << ", LUX_SM_UNLIT, 1.0);\n";
                out << "    gEmissiveAo      = vec4(" << emitter.attr("emissive") << ", 1.0);\n";
            }
            else
            {
                out << "    vec3 _Ngb = " << normal << ";\n";
                out << "    if (!gl_FrontFacing) _Ngb = -_Ngb;\n";
                out << "    gAlbedoMetallic  = vec4(" << emitter.attr("base_color") << ", " << emitter.attr("metallic")
                    << ");\n";
                out << "    gNormalRoughness = packGNormalSM(_Ngb, LUX_SM_PBR, " << emitter.attr("roughness") << ");\n";
                out << "    gEmissiveAo      = vec4(" << emitter.attr("emissive") << ", " << emitter.attr("ao")
                    << ");\n";
            }
        }
        else if (forward)
        {
            out << "    outColor = vec4(" << emitter.attr("base_color") << " + " << emitter.attr("emissive") << ", "
                << emitter.attr("opacity") << ");\n";
        }
        out << "}\n";
        return out.str();
    }

} // namespace lux::shadergen::glsl
