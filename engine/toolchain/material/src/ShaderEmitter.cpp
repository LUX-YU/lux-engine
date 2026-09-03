// =============================================================================
//  ShaderEmitter.cpp  --  ShaderIR -> GLSL fragment source (pure, no shaderc/no device)
// -----------------------------------------------------------------------------
//  Walks the SSA values in topological order, emitting one GLSL local per value
//  (giving automatic CSE), then emits the shell epilogue based on pass/shading_model.
//  Matches the behavior of the old material_graph_glsl emitter, but:
//    - it consumes the generic ShaderIR: Input references come from
//      self-contained InputSlot entries (name/type/location), and outputs are
//      looked up by name (falling back to Output.dflt when value_id==kNoValue);
//    - the shell #includes the real single-source-of-truth file
//      (gbuffer_encode.glsl) instead of hand-copying it inline (ShaderCompiler
//      wires up an IncluderInterface for this).
//
//  Currently supported: GBuffer (PBR/Unlit) and Forward Unlit. textures/params,
//  TbnNormal, RawExpr, and Forward lighting are not yet implemented and will be
//  added later; emitGlsl fails explicitly for them.
// =============================================================================

#include <lux/engine/material/compiler/Backend.hpp>

#include <array>
#include <charconv>
#include <map>
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
                s += ".0";
            return s;
        }

        std::string literal(EValueType t, const float* c)
        {
            const int n = arity(t);
            if (n == 1)
                return fmtF(c[0]);
            std::string s = glslType(t);
            s += "(";
            for (int i = 0; i < n; ++i)
            {
                if (i)
                    s += ", ";
                s += fmtF(c[i]);
            }
            s += ")";
            return s;
        }

        // InputSlot.name (contract glsl_name) -> the interpolant variable name used by
        // the engine material's vertex shader. The backend-neutral core keeps
        // InputSlot.name as a generic glsl_name; the "engine uses vXxx" convention
        // belongs to the material client (the engine half) and lives here -- this lets
        // the forward shell #include lighting_common.glsl and reuse its input
        // declarations, and keeps GBuffer's input naming consistent with the engine's
        // gbuffer fragment shader.
        std::string interpName(const std::string& glsl_name)
        {
            if (glsl_name == "world_position")
                return "vWorldPos";
            if (glsl_name == "world_normal")
                return "vWorldNormal";
            if (glsl_name == "uv0")
                return "vUV";
            if (glsl_name == "world_tangent")
                return "vWorldTangent";
            return glsl_name;
        }

        struct Emitter
        {
            const ShaderIR& ir;
            std::string* error;
            bool ok = true;
            bool tex_mode = false; // Set when there's a SampleTexture: needs set-2 uTex[] + nonuniform
            bool need_mat = false; // Set when there's tex/param: needs the set-4 GraphMaterialGPU SSBO + vMatIndex
            std::ostringstream body;
            // Shading inputs actually used: location -> (glslType, name). Using a
            // map guarantees declared_in iterates in ascending location order.
            std::map<int, std::pair<std::string, std::string>> declared_in;

            Emitter(const ShaderIR& ir_, std::string* e_)
                : ir(ir_),
                  error(e_)
            {}

            bool fail(std::string m)
            {
                if (error && ok)
                    *error = std::move(m);
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
                    return;
                // WorldTangent special case: the contract type is Vec4 (including the
                // handedness w), but the engine material vertex shader's interpolant
                // is vec3 (the old emitter also used vec3@5). Declare it as vec3 to
                // match the vertex shader; at the Input reference site we wrap it
                // back into vec4(world_tangent, 1.0).
                const char* type = (s.name == "world_tangent") ? "vec3" : glslType(s.type);
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

            bool emitValues()
            {
                // Per-material data (textures/params) is fetched from the set-4
                // GraphMaterialGPU SSBO, indexed by the flat vMatIndex
                // interpolant (matching gbuffer_pbr.frag).
                if (need_mat)
                {
                    declared_in[4] = {"flat uint", "vMatIndex"};
                    body << "    GraphMaterialGPU _mat = uMats.mats[vMatIndex];\n";
                }
                for (size_t i = 0; i < ir.values.size(); ++i)
                {
                    const ShaderIRValue& v = ir.values[i];
                    std::string rhs;
                    switch (v.op)
                    {
                    case EOp::CONSTANT:
                        rhs = literal(v.type, v.constant);
                        break;
                    case EOp::INPUT: {
                        if (v.slot >= ir.inputs.size())
                        {
                            fail("shadergen: Input slot out of range");
                            break;
                        }
                        const InputSlot& s = ir.inputs[v.slot];
                        useInput(s);
                        // The WorldTangent interpolant is vec3, but the contract/
                        // downstream code wants Vec4 -> wrap it with handedness w=1.
                        rhs = (s.name == "world_tangent" && s.type == EValueType::VEC4) ? "vec4(vWorldTangent, 1.0)"
                                                                                        : interpName(s.name);
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
                    case EOp::SWIZZLE: {
                        std::string sw;
                        for (int k = 0; k < arity(v.type); ++k)
                            sw += "xyzw"[v.swizzle[k] & 3u];
                        rhs = operand(v, 0) + "." + sw;
                        break;
                    }
                    case EOp::CONSTRUCT: {
                        rhs = std::string(glslType(v.type)) + "(";
                        bool first = true;
                        for (int k = 0; k < 4; ++k)
                        {
                            if (v.operands[k] == kNoValue)
                                break;
                            if (!first)
                                rhs += ", ";
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
                        rhs = "luxSampleTexture(_mat.tex[" + std::to_string(v.slot) + "], " + operand(v, 0) + ")";
                        break;
                    case EOp::PARAM: {
                        // Reads one param lane from the Graph-family SSBO
                        // (slot i -> params[i]), then swizzles it down to the
                        // declared arity.
                        const int n = arity(v.type);
                        const char* sw = (n == 1) ? "x" : (n == 2) ? "xy" : (n == 3) ? "xyz" : "xyzw";
                        rhs = "_mat.params[" + std::to_string(v.slot) + "]." + sw;
                        break;
                    }
                    case EOp::TBN_NORMAL: {
                        // Tangent-space normal -> world (mat3(T,B,N) * n), using the
                        // interpolated T/B/N (mirroring the engine's
                        // calculateWorldNormal). Declares the interpolant directly:
                        // world_bitangent@6 is not an EMaterialInput, and the old
                        // emitter also declared it directly; using the world_*
                        // naming consistently lets it dedupe against the Input
                        // slots (world_normal/world_tangent) without causing a
                        // location conflict.
                        declared_in[1] = {"vec3", "vWorldNormal"};
                        declared_in[5] = {"vec3", "vWorldTangent"};
                        declared_in[6] = {"vec3", "vWorldBitangent"};
                        // Tangent-space normal -> world. The 2*rgb-1 decode is the
                        // SEPARATE upstream DecodeNormal op (EOp::DECODE_NORMAL); do NOT
                        // re-decode here or a DecodeNormal->TbnTransform chain double-decodes.
                        rhs = "normalize(mat3(normalize(vWorldTangent), normalize(vWorldBitangent), "
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
                        return false;
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
                        continue;
                    if (o.value_id != kNoValue && o.value_id < ir.values.size())
                        return vname(o.value_id);
                    return literal(o.type, o.dflt);
                }
                return "0.0"; // outputs always contains every material property; this should be unreachable in practice
            }

            bool hasBoundOutput(const char* name) const
            {
                for (const auto& o : ir.outputs)
                    if (o.name == name)
                        return o.value_id != kNoValue && o.value_id < ir.values.size();
                return false;
            }
        };
    } // namespace

    lux::cxx::expected<std::string, std::string> emitGlsl(const ShaderIR& ir, const EmitParams& p)
    {
        std::string error;

        if (p.pass == EMaterialPass::SHADOW || p.pass == EMaterialPass::VIS_BUFFER)
            return lux::cxx::unexpected(std::string("shadergen: Shadow/VisBuffer passes not supported yet"));

        const bool gbuffer = p.pass == EMaterialPass::GBUFFER;
        const bool unlit = p.shading_model == rdesc::ELightingTechnique::Unlit;
        const bool forward_lit = !gbuffer && !unlit;
        const bool forward_toon = forward_lit && p.shading_model == rdesc::ELightingTechnique::Stylized;

        bool tex_mode = false, param_mode = false;
        for (const auto& v : ir.values)
        {
            if (v.op == EOp::SAMPLE_TEXTURE)
                tex_mode = true;
            if (v.op == EOp::PARAM)
                param_mode = true;
        }

        Emitter em{ir, &error};
        em.tex_mode = tex_mode;
        em.need_mat = tex_mode || param_mode; // Any per-material data -> Graph family
        if (!em.emitValues())
            return lux::cxx::unexpected(error.empty() ? std::string("shadergen: emitValues failed") : std::move(error));

        // Normal: GBuffer/Forward-lit use the graph's NormalTS (if bound),
        // otherwise the interpolated world normal; Unlit uses the world normal.
        std::string normalExpr;
        if (gbuffer || forward_lit)
        {
            const bool normal_bound = !unlit && em.hasBoundOutput("normal_ts");
            normalExpr =
                normal_bound ? "normalize(" + em.attr("normal_ts") + ")" : "normalize(" + em.ensureWorldNormal() + ")";
        }

        // Transition coverage is a fixed mesh-pass contract, not an optional
        // material-graph input. The shared mesh vertex shaders always emit
        // locations 7..9 and every material family must apply the same stable
        // parent/child dither. Forward-lit graphs inherit these declarations
        // from lighting_common; compact GBuffer/Forward-Unlit shells declare
        // them explicitly even when the authored graph has no input nodes.
        if (!forward_lit)
        {
            em.declared_in[7] = {"flat float", "vTransitionCoverage"};
            em.declared_in[8] = {"flat uint", "vTransitionSeed"};
            em.declared_in[9] = {"flat uint", "vTransitionFadeOut"};
        }
        const char* gbuffer_sm_id =
            p.shading_model == rdesc::ELightingTechnique::Stylized ? "LUX_SM_TOON" : "LUX_SM_PBR";

        std::ostringstream out;
        out << "#version 450\n";
        if (em.tex_mode ||
            forward_lit) // forward's lighting_common always needs this extension for the unbounded uTex[]
            out << "#extension GL_EXT_nonuniform_qualifier : require\n";

        if (forward_lit)
        {
            // Forward shell: #includes the aggregated single-source-of-truth
            // lighting_common (which provides the FS input / set-0,2,3
            // bindings / push constant / ViewGpuData / kAmbient /
            // material_types / uTex), so this does NOT declare its own
            // input/out/uTex/material_types -- it only adds the set-4 Graph SSBO.
            out << "#include \"lighting_common.glsl\"\n";
            out << "#ifndef LUX_SHADOW_TECHNIQUE_ID\n#define LUX_SHADOW_TECHNIQUE_ID 0\n#endif\n";
            out << "#if LUX_SHADOW_TECHNIQUE_ID == 0\n"
                << "#include \"shadow_pcf.glsl\"\n"
                << "#elif LUX_SHADOW_TECHNIQUE_ID == 1\n"
                << "#include \"shadow_evsm.glsl\"\n"
                << "#else\n#error \"Unknown LUX_SHADOW_TECHNIQUE_ID\"\n#endif\n";
            out << "#include \"brdf/brdf_common.glsl\"\n";
            out << (forward_toon ? "#include \"brdf/brdf_toon.glsl\"\n" : "#include \"brdf/brdf_ggx.glsl\"\n");
            if (em.need_mat)
                out << "layout(set = 4, binding = 4, std430) readonly buffer MatBuf {\n"
                    << "    GraphMaterialGPU mats[];\n"
                    << "} uMats;\n";
        }
        else // GBuffer / Forward-Unlit: declares its own input + out + resource blocks.
        {
            for (const auto& [loc, tn] : em.declared_in)
                out << "layout(location = " << loc << ") in " << tn.first << " " << tn.second << ";\n";

            if (gbuffer)
            {
                out << "layout(location = 0) out vec4 gAlbedoMetallic;\n";
                out << "layout(location = 1) out vec4 gNormalRoughness;\n";
                out << "layout(location = 2) out vec4 gEmissiveAo;\n";
            }
            else
            {
                out << "layout(location = 0) out vec4 outColor;\n";
            }

            out << "#include \"transition_dither.glsl\"\n";

            // Per-material data (Graph family): #include the real single-source-of-truth
            // material_types.glsl to get GraphMaterialGPU, then declare the set-2
            // bindless sampler plus the set-4 SSBO (binding 4).
            if (em.need_mat)
            {
                out << "#include \"material_types.glsl\"\n";
                if (em.tex_mode)
                {
                    out << "layout(set = 2, binding = 0) uniform sampler2D uTex[];\n";
                    out << "#include \"texture_sampling.glsl\"\n";
                }
                out << "layout(set = 4, binding = 4, std430) readonly buffer MatBuf {\n"
                    << "    GraphMaterialGPU mats[];\n"
                    << "} uMats;\n";
            }
            if (gbuffer)
            {
                // The real single-source-of-truth, resolved by the Includer rather than copied inline.
                out << "#include \"gbuffer_encode.glsl\"\n";
            }
        }

        out << "\nvoid main()\n{\n";
        out << "    luxApplyDirectedTransitionCoverage(\n"
               "        vTransitionCoverage, vTransitionSeed, vTransitionFadeOut != 0u);\n";
        out << em.body.str();

        if (p.alpha_mode == rdesc::EAlphaMode::Mask)
            out << "    if (" << em.attr("opacity") << " < " << fmtF(p.alpha_cutoff) << ") discard;\n";
        else if (p.alpha_mode == rdesc::EAlphaMode::Blend)
            // The deferred renderer has no transparent pass yet, so a Blend material
            // would otherwise write a SOLID opaque block. Fall back to an alpha-test
            // CUTOUT (hard edges, but the silhouette is correct + visible) so blended
            // hair/eyelash cards show up. A future transparent forward pass replaces
            // this with real back-to-front blending.
            out << "    if (" << em.attr("opacity") << " < 0.5) discard;\n";

        if (gbuffer)
        {
            if (unlit)
            {
                out << "    gAlbedoMetallic  = vec4(" << em.attr("base_color") << ", 0.0);\n";
                out << "    gNormalRoughness = packGNormalSM(" << normalExpr << ", LUX_SM_UNLIT, 1.0);\n";
                out << "    gEmissiveAo      = vec4(" << em.attr("emissive") << ", 1.0);\n";
            }
            else
            {
                out << "    vec3 _Ngb = " << normalExpr << ";\n";
                out << "    if (!gl_FrontFacing) _Ngb = -_Ngb;\n";
                out << "    gAlbedoMetallic  = vec4(" << em.attr("base_color") << ", " << em.attr("metallic") << ");\n";
                out << "    gNormalRoughness = packGNormalSM(_Ngb, " << gbuffer_sm_id << ", " << em.attr("roughness")
                    << ");\n";
                out << "    gEmissiveAo      = vec4(" << em.attr("emissive") << ", " << em.attr("ao") << ");\n";
            }
        }
        // Forward Stylized (cel/toon): quantized diffuse and specular band, matching the old emitter.
        else if (forward_toon)
        {
            out << "    vec3  _albedo = " << em.attr("base_color") << ";\n";
            out << "    vec3  _N = " << normalExpr << ";\n";
            out << "    if (!gl_FrontFacing) _N = -_N;\n";
            out << "    vec3  _V = normalize(vViewDir);\n";
            out << "    float _vd = computeViewDepth(vWorldPos);\n";
            out << "    const float _steps = 4.0;\n";
            out << "    vec3  _Lo = vec3(0.0);\n";
            out << "    for (int i = 0; i < uDirectionalLights.lights.length(); ++i) {\n";
            out << "        vec3 _Ld = uDirectionalLights.lights[i].direction;\n";
            out << "        if (dot(_Ld, _Ld) < 1e-8) continue;\n";
            out << "        vec3 _L = normalize(-_Ld);\n";
            out << "        vec3 _lc = uDirectionalLights.lights[i].color * uDirectionalLights.lights[i].intensity;\n";
            out << "        float _sh = directionalShadow(uint(i), vWorldPos, _N, _vd);\n";
            out << "        _Lo += _albedo * _lc * toonStep(max(dot(_N, _L), 0.0), _steps) * _sh;\n";
            out << "        vec3 _H = normalize(_L + _V);\n";
            out << "        _Lo += _lc * toonStep(pow(max(dot(_N, _H), 0.0), 32.0), _steps) * _sh;\n";
            out << "    }\n";
            out << "    for (int i = 0; i < uPointLights.lights.length(); ++i) {\n";
            out << "        if (uPointLights.lights[i].range <= 0.0) continue;\n";
            out << "        vec3 _lv = luxPointLightViewPosition(uint(i)) - vWorldPos;\n";
            out << "        float _d = length(_lv);\n";
            out << "        if (_d > uPointLights.lights[i].range) continue;\n";
            out << "        vec3 _L = _lv / _d;\n";
            out << "        float _att = 1.0 / (uPointLights.lights[i].attenuation_constant"
                   " + uPointLights.lights[i].attenuation_linear * _d"
                   " + uPointLights.lights[i].attenuation_quadratic * _d * _d);\n";
            out << "        vec3 _lc = uPointLights.lights[i].color * uPointLights.lights[i].intensity * _att;\n";
            out << "        float _sh = ((uPointLights.lights[i].flags & 1u) != 0u) ? pointShadow(uint(i), vWorldPos, "
                   "_N) : 1.0;\n";
            out << "        _Lo += _albedo * _lc * toonStep(max(dot(_N, _L), 0.0), _steps) * _sh;\n";
            out << "    }\n";
            out << "    outColor = vec4(kAmbient * _albedo + _Lo + " << em.attr("emissive") << ", "
                << em.attr("opacity") << ");\n";
        }
        else if (forward_lit) // Forward GGX (PBR): Cook-Torrance over set-3 lights, replicating the old emitter.
        {
            out << "    vec3  _albedo    = " << em.attr("base_color") << ";\n";
            out << "    float _metallic  = " << em.attr("metallic") << ";\n";
            out << "    float _roughness = max(" << em.attr("roughness") << ", 0.04);\n";
            out << "    vec3  _N = " << normalExpr << ";\n";
            out << "    if (!gl_FrontFacing) _N = -_N;\n";
            out << "    vec3  _V = normalize(vViewDir);\n";
            out << "    float _NdotV = max(dot(_N, _V), 0.0);\n";
            out << "    vec3  _F0 = mix(vec3(0.04), _albedo, _metallic);\n";
            out << "    float _vd = computeViewDepth(vWorldPos);\n";
            out << "    vec3  _Lo = vec3(0.0);\n";
            out << "    for (int i = 0; i < uDirectionalLights.lights.length(); ++i) {\n";
            out << "        vec3 _Ld = uDirectionalLights.lights[i].direction;\n";
            out << "        if (dot(_Ld, _Ld) < 1e-8) continue;\n";
            out << "        vec3 _L = normalize(-_Ld);\n";
            out << "        vec3 _lc = uDirectionalLights.lights[i].color * uDirectionalLights.lights[i].intensity;\n";
            out << "        float _sh = directionalShadow(uint(i), vWorldPos, _N, _vd);\n";
            out << "        _Lo += pbrDirectLight(_albedo, _metallic, _roughness, _F0, _N, _V, _NdotV, _L, _lc) * "
                   "_sh;\n";
            out << "    }\n";
            out << "    for (int i = 0; i < uPointLights.lights.length(); ++i) {\n";
            out << "        if (uPointLights.lights[i].range <= 0.0) continue;\n";
            out << "        vec3 _lv = luxPointLightViewPosition(uint(i)) - vWorldPos;\n";
            out << "        float _d = length(_lv);\n";
            out << "        if (_d > uPointLights.lights[i].range) continue;\n";
            out << "        vec3 _L = _lv / _d;\n";
            out << "        float _att = 1.0 / (uPointLights.lights[i].attenuation_constant"
                   " + uPointLights.lights[i].attenuation_linear * _d"
                   " + uPointLights.lights[i].attenuation_quadratic * _d * _d);\n";
            out << "        vec3 _lc = uPointLights.lights[i].color * uPointLights.lights[i].intensity * _att;\n";
            out << "        float _sh = ((uPointLights.lights[i].flags & 1u) != 0u) ? pointShadow(uint(i), vWorldPos, "
                   "_N) : 1.0;\n";
            out << "        _Lo += pbrDirectLight(_albedo, _metallic, _roughness, _F0, _N, _V, _NdotV, _L, _lc) * "
                   "_sh;\n";
            out << "    }\n";
            out << "    outColor = vec4(kAmbient * _albedo + _Lo + " << em.attr("emissive") << ", "
                << em.attr("opacity") << ");\n";
        }
        else // Forward Unlit: passthrough (base + emissive).
        {
            out << "    outColor = vec4(" << em.attr("base_color") << " + " << em.attr("emissive") << ", "
                << em.attr("opacity") << ");\n";
        }

        out << "}\n";
        return out.str();
    }

} // namespace lux::shadergen::glsl
