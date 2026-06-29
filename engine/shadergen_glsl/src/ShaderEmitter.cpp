// =============================================================================
//  ShaderEmitter.cpp  —  ShaderIR -> GLSL fragment 源（纯，无 shaderc/无设备）
// -----------------------------------------------------------------------------
//  遍历拓扑序 SSA，每值发射一个 GLSL local（自动 CSE），再按 pass/shading_model
//  发射外壳 epilogue。与旧 material_graph_glsl emitter 行为对齐，但：
//    - 吃通用 ShaderIR：Input 引用来自 InputSlot 槽（name/type/location 自包含），
//      输出按命名 Output 取（value_id==kNoValue 用 Output.dflt）；
//    - 外壳 #include 真 SSOT（gbuffer_encode.glsl），不内联手抄（ShaderCompiler 配
//      了 IncluderInterface）。
//
//  M1 范围：GBuffer（PBR/Unlit）+ Forward Unlit。textures/params/TbnNormal/RawExpr/
//  Forward 光照 在后续里程碑，emitGlsl 对它们明确报错。
// =============================================================================

#include <lux/engine/shadergen/glsl/Backend.hpp>

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
            case EValueType::Float: return "float";
            case EValueType::Vec2:  return "vec2";
            case EValueType::Vec3:  return "vec3";
            case EValueType::Vec4:  return "vec4";
            }
            return "float";
        }

        int arity(EValueType t)
        {
            switch (t)
            {
            case EValueType::Float: return 1;
            case EValueType::Vec2:  return 2;
            case EValueType::Vec3:  return 3;
            case EValueType::Vec4:  return 4;
            }
            return 1;
        }

        // 最短往返 float 字面量，必带小数点（保证是 float 而非 int）。
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
                if (i) s += ", ";
                s += fmtF(c[i]);
            }
            s += ")";
            return s;
        }

        // InputSlot.name(契约 glsl_name) -> 引擎材质 vertex shader 的 interpolant 变量名。
        // 中性核 InputSlot.name 保持通用 glsl_name；"引擎用 vXxx" 是材质客户（engine 半）
        // 的约定，放这里 —— 让 forward 外壳能 #include lighting_common.glsl 复用其 input 声明，
        // 也让 GBuffer 的 input 命名收敛到引擎 gbuffer frag 一致。
        std::string interpName(const std::string& glsl_name)
        {
            if (glsl_name == "world_position") return "vWorldPos";
            if (glsl_name == "world_normal")   return "vWorldNormal";
            if (glsl_name == "uv0")            return "vUV";
            if (glsl_name == "world_tangent")  return "vWorldTangent";
            return glsl_name;
        }

        struct Emitter
        {
            const ShaderIR& ir;
            std::string*    error;
            bool            ok = true;
            bool            tex_mode = false;  // 有 SampleTexture：set-2 uTex[] + nonuniform
            bool            need_mat = false;  // 有 tex/param：set-4 GraphMaterialGPU SSBO + vMatIndex
            std::ostringstream body;
            // 用到的着色输入：location -> (glslType, name)。map 保证 declIn 按 location 升序。
            std::map<int, std::pair<std::string, std::string>> declared_in;

            Emitter(const ShaderIR& ir_, std::string* e_) : ir(ir_), error(e_) {}

            bool fail(std::string m)
            {
                if (error && ok) *error = std::move(m);
                ok = false;
                return false;
            }

            static std::string vname(uint32_t i) { return "v" + std::to_string(i); }

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
                // WorldTangent 特例：契约类型是 Vec4（含手性 w），但引擎材质 vert 的
                // interpolant 是 vec3（旧 emitter 也是 vec3@5）。声明 vec3 以匹配 vert；
                // Input 引用处再包成 vec4(world_tangent, 1.0)。
                const char* type = (s.name == "world_tangent") ? "vec3" : glslType(s.type);
                declared_in[s.location] = { type, interpName(s.name) };
            }

            // epilogue 需要世界法线兜底（PBR/GBuffer normalExpr fallback / Unlit normal）。
            // 强制声明在 location 1（与材质 vert 一致）；若图也用了 Input(WorldNormal)，
            // 同 location/name 自然去重。
            std::string ensureWorldNormal()
            {
                declared_in[1] = { "vec3", "vWorldNormal" };
                return "vWorldNormal";
            }

            bool emitValues()
            {
                // per-material 数据（textures/params）从 set-4 GraphMaterialGPU SSBO 取，
                // 由 flat vMatIndex interpolant 索引（与 gbuffer_pbr.frag 一致）。
                if (need_mat)
                {
                    declared_in[4] = { "flat uint", "vMatIndex" };
                    body << "    GraphMaterialGPU _mat = uMats.mats[vMatIndex];\n";
                }
                for (size_t i = 0; i < ir.values.size(); ++i)
                {
                    const ShaderIRValue& v = ir.values[i];
                    std::string rhs;
                    switch (v.op)
                    {
                    case EOp::Constant:     rhs = literal(v.type, v.constant); break;
                    case EOp::Input:
                    {
                        if (v.slot >= ir.inputs.size()) { fail("shadergen: Input slot out of range"); break; }
                        const InputSlot& s = ir.inputs[v.slot];
                        useInput(s);
                        // WorldTangent interpolant 是 vec3，契约/下游要 Vec4 -> 包手性 w=1。
                        rhs = (s.name == "world_tangent" && s.type == EValueType::Vec4)
                                  ? "vec4(vWorldTangent, 1.0)"
                                  : interpName(s.name);
                        break;
                    }
                    case EOp::Mul:          rhs = operand(v, 0) + " * " + operand(v, 1); break;
                    case EOp::Add:          rhs = operand(v, 0) + " + " + operand(v, 1); break;
                    case EOp::Sub:          rhs = operand(v, 0) + " - " + operand(v, 1); break;
                    case EOp::Div:          rhs = operand(v, 0) + " / " + operand(v, 1); break;
                    case EOp::Dot:          rhs = "dot(" + operand(v, 0) + ", " + operand(v, 1) + ")"; break;
                    case EOp::Min:          rhs = "min(" + operand(v, 0) + ", " + operand(v, 1) + ")"; break;
                    case EOp::Max:          rhs = "max(" + operand(v, 0) + ", " + operand(v, 1) + ")"; break;
                    case EOp::Pow:          rhs = "pow(" + operand(v, 0) + ", " + operand(v, 1) + ")"; break;
                    case EOp::Step:         rhs = "step(" + operand(v, 0) + ", " + operand(v, 1) + ")"; break;
                    case EOp::Mod:          rhs = "mod(" + operand(v, 0) + ", " + operand(v, 1) + ")"; break;
                    case EOp::Cross:        rhs = "cross(" + operand(v, 0) + ", " + operand(v, 1) + ")"; break;
                    case EOp::Reflect:      rhs = "reflect(" + operand(v, 0) + ", " + operand(v, 1) + ")"; break;
                    case EOp::Lerp:         rhs = "mix(" + operand(v, 0) + ", " + operand(v, 1) + ", " + operand(v, 2) + ")"; break;
                    case EOp::Saturate:     rhs = "clamp(" + operand(v, 0) + ", 0.0, 1.0)"; break;
                    case EOp::OneMinus:     rhs = "1.0 - " + operand(v, 0); break;
                    case EOp::Abs:          rhs = "abs(" + operand(v, 0) + ")"; break;
                    case EOp::Sqrt:         rhs = "sqrt(" + operand(v, 0) + ")"; break;
                    case EOp::Floor:        rhs = "floor(" + operand(v, 0) + ")"; break;
                    case EOp::Fract:        rhs = "fract(" + operand(v, 0) + ")"; break;
                    case EOp::Sin:          rhs = "sin(" + operand(v, 0) + ")"; break;
                    case EOp::Cos:          rhs = "cos(" + operand(v, 0) + ")"; break;
                    case EOp::Normalize:    rhs = "normalize(" + operand(v, 0) + ")"; break;
                    case EOp::Length:       rhs = "length(" + operand(v, 0) + ")"; break;
                    case EOp::DecodeNormal: rhs = "normalize(" + operand(v, 0) + ".xyz * 2.0 - 1.0)"; break;
                    case EOp::Swizzle:
                    {
                        std::string sw;
                        for (int k = 0; k < arity(v.type); ++k)
                            sw += "xyzw"[v.swizzle[k] & 3u];
                        rhs = operand(v, 0) + "." + sw;
                        break;
                    }
                    case EOp::Construct:
                    {
                        rhs = std::string(glslType(v.type)) + "(";
                        bool first = true;
                        for (int k = 0; k < 4; ++k)
                        {
                            if (v.operands[k] == kNoValue) break;
                            if (!first) rhs += ", ";
                            rhs += operand(v, k);
                            first = false;
                        }
                        rhs += ")";
                        break;
                    }
                    // M1 未支持：需要 set2/set4 数据绑定 / 额外 interpolant / 后端文本。
                    case EOp::SampleTexture:
                        // set-2 bindless 采样：index = per-material tex[slot].bindless。
                        rhs = "texture(uTex[nonuniformEXT(_mat.tex[" + std::to_string(v.slot) +
                              "].bindless)], " + operand(v, 0) + ")";
                        break;
                    case EOp::Param:
                    {
                        // 从 Graph-family SSBO 读一条 param lane（slot i -> params[i]），
                        // swizzle 到声明的 arity。
                        const int   n  = arity(v.type);
                        const char* sw = (n == 1) ? "x" : (n == 2) ? "xy" : (n == 3) ? "xyz" : "xyzw";
                        rhs = "_mat.params[" + std::to_string(v.slot) + "]." + sw;
                        break;
                    }
                    case EOp::TbnNormal:
                    {
                        // 切线空间法线 -> 世界（mat3(T,B,N) * n），用插值的 T/B/N（镜像引擎
                        // calculateWorldNormal）。直接声明 interpolant：world_bitangent@6 不是
                        // EMaterialInput，旧 emitter 也直接声明；统一用 world_* 名以与 Input
                        // 槽（world_normal/world_tangent）去重，不产生 location 冲突。
                        declared_in[1] = { "vec3", "vWorldNormal" };
                        declared_in[5] = { "vec3", "vWorldTangent" };
                        declared_in[6] = { "vec3", "vWorldBitangent" };
                        // Tangent-space normal -> world. The 2*rgb-1 decode is the
                        // SEPARATE upstream DecodeNormal op (EOp::DecodeNormal); do NOT
                        // re-decode here or a DecodeNormal->TbnTransform chain double-decodes.
                        rhs = "normalize(mat3(normalize(vWorldTangent), normalize(vWorldBitangent), "
                              "normalize(vWorldNormal)) * " + operand(v, 0) + ")";
                        break;
                    }
                    case EOp::RawExpr:       return fail("shadergen(M1): RawExpr 暂不支持（逃生舱后续）");
                    default:                 return fail("shadergen: unsupported ShaderIR op");
                    }
                    if (!ok) return false;
                    body << "    " << glslType(v.type) << " " << vname(static_cast<uint32_t>(i))
                         << " = " << rhs << ";\n";
                }
                return ok;
            }

            // 命名输出：value_id 有效用其 local，否则用自包含的 dflt。
            std::string attr(const char* name) const
            {
                for (const auto& o : ir.outputs)
                {
                    if (o.name != name) continue;
                    if (o.value_id != kNoValue && o.value_id < ir.values.size())
                        return vname(o.value_id);
                    return literal(o.type, o.dflt);
                }
                return "0.0";  // outputs 恒含全部材质属性；理论上不达
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

    lux::cxx::expected<std::string, std::string>
    emitGlsl(const ShaderIR& ir, const EmitParams& p)
    {
        std::string error;

        if (p.pass == rdesc::EMaterialPass::Shadow || p.pass == rdesc::EMaterialPass::VisBuffer)
            return lux::cxx::unexpected(std::string("shadergen: Shadow/VisBuffer passes not supported yet"));

        const bool gbuffer = (p.pass == rdesc::EMaterialPass::GBuffer);
        const bool unlit   = (p.shading_model == rdesc::EMaterialShadingModel::Unlit);
        const bool forward_lit  = !gbuffer && !unlit;
        const bool forward_toon = forward_lit && (p.shading_model == rdesc::EMaterialShadingModel::Stylized);

        bool tex_mode = false, param_mode = false;
        for (const auto& v : ir.values)
        {
            if (v.op == EOp::SampleTexture) tex_mode   = true;
            if (v.op == EOp::Param)         param_mode = true;
        }

        Emitter em{ ir, &error };
        em.tex_mode = tex_mode;
        em.need_mat = tex_mode || param_mode;   // 任何 per-material 数据 -> Graph family
        if (!em.emitValues())
            return lux::cxx::unexpected(error.empty() ? std::string("shadergen: emitValues failed") : std::move(error));

        // 法线：GBuffer/Forward-lit 用图的 NormalTS（若绑定），否则插值世界法线；Unlit 用世界法线。
        std::string normalExpr;
        if (gbuffer || forward_lit)
        {
            const bool normal_bound = !unlit && em.hasBoundOutput("normal_ts");
            normalExpr = normal_bound ? "normalize(" + em.attr("normal_ts") + ")"
                                      : "normalize(" + em.ensureWorldNormal() + ")";
        }
        const char* gbuffer_sm_id =
            (p.shading_model == rdesc::EMaterialShadingModel::Stylized) ? "LUX_SM_TOON" : "LUX_SM_PBR";

        std::ostringstream out;
        out << "#version 450\n";
        if (em.tex_mode || forward_lit)   // forward 的 lighting_common 无界 uTex[] 恒需该扩展
            out << "#extension GL_EXT_nonuniform_qualifier : require\n";

        if (forward_lit)
        {
            // Forward 外壳：#include 聚合 SSOT lighting_common（提供 FS input / set-0,2,3
            // binding / push constant / ViewGpuData / kAmbient / material_types / uTex），
            // 故【不自声明 input/out/uTex/material_types】，只补 set-4 Graph SSBO。
            out << "#include \"lighting_common.glsl\"\n";
            out << "#ifndef LUX_SHADOW_TECHNIQUE_ID\n#define LUX_SHADOW_TECHNIQUE_ID 0\n#endif\n";
            out << "#if LUX_SHADOW_TECHNIQUE_ID == 0\n"
                << "#include \"shadow_pcf.glsl\"\n"
                << "#elif LUX_SHADOW_TECHNIQUE_ID == 1\n"
                << "#include \"shadow_evsm.glsl\"\n"
                << "#else\n#error \"Unknown LUX_SHADOW_TECHNIQUE_ID\"\n#endif\n";
            out << "#include \"brdf/brdf_common.glsl\"\n";
            out << (forward_toon ? "#include \"brdf/brdf_toon.glsl\"\n"
                                 : "#include \"brdf/brdf_ggx.glsl\"\n");
            if (em.need_mat)
                out << "layout(set = 4, binding = 4, std430) readonly buffer MatBuf {\n"
                    << "    GraphMaterialGPU mats[];\n"
                    << "} uMats;\n";
        }
        else  // GBuffer / Forward-Unlit：自声明 input + out + 资源块。
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

            // per-material 数据（Graph family）：#include 真 SSOT material_types.glsl 拿
            // GraphMaterialGPU，再声明 set-2 bindless 采样器 + set-4 SSBO（binding 4）。
            if (em.need_mat)
            {
                out << "#include \"material_types.glsl\"\n";
                if (em.tex_mode)
                    out << "layout(set = 2, binding = 0) uniform sampler2D uTex[];\n";
                out << "layout(set = 4, binding = 4, std430) readonly buffer MatBuf {\n"
                    << "    GraphMaterialGPU mats[];\n"
                    << "} uMats;\n";
            }
            if (gbuffer)
                out << "#include \"gbuffer_encode.glsl\"\n";   // 真 SSOT，Includer 解析（不内联手抄）
        }

        out << "\nvoid main()\n{\n";
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
                out << "    gNormalRoughness = packGNormalSM(_Ngb, " << gbuffer_sm_id << ", " << em.attr("roughness") << ");\n";
                out << "    gEmissiveAo      = vec4(" << em.attr("emissive") << ", " << em.attr("ao") << ");\n";
            }
        }
        else if (forward_toon)  // Forward Stylized（cel/toon）：量化 diffuse + spec band，复刻旧 emitter。
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
            out << "        vec3 _lv = uPointLights.lights[i].position - vWorldPos;\n";
            out << "        float _d = length(_lv);\n";
            out << "        if (_d > uPointLights.lights[i].range) continue;\n";
            out << "        vec3 _L = _lv / _d;\n";
            out << "        float _att = 1.0 / (uPointLights.lights[i].attenuation_constant"
                   " + uPointLights.lights[i].attenuation_linear * _d"
                   " + uPointLights.lights[i].attenuation_quadratic * _d * _d);\n";
            out << "        vec3 _lc = uPointLights.lights[i].color * uPointLights.lights[i].intensity * _att;\n";
            out << "        float _sh = ((uPointLights.lights[i].flags & 1u) != 0u) ? pointShadow(uint(i), vWorldPos, _N) : 1.0;\n";
            out << "        _Lo += _albedo * _lc * toonStep(max(dot(_N, _L), 0.0), _steps) * _sh;\n";
            out << "    }\n";
            out << "    outColor = vec4(kAmbient * _albedo + _Lo + " << em.attr("emissive")
                << ", " << em.attr("opacity") << ");\n";
        }
        else if (forward_lit)  // Forward GGX（PBR）：Cook-Torrance over set-3 lights，复刻旧 emitter。
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
            out << "        _Lo += pbrDirectLight(_albedo, _metallic, _roughness, _F0, _N, _V, _NdotV, _L, _lc) * _sh;\n";
            out << "    }\n";
            out << "    for (int i = 0; i < uPointLights.lights.length(); ++i) {\n";
            out << "        if (uPointLights.lights[i].range <= 0.0) continue;\n";
            out << "        vec3 _lv = uPointLights.lights[i].position - vWorldPos;\n";
            out << "        float _d = length(_lv);\n";
            out << "        if (_d > uPointLights.lights[i].range) continue;\n";
            out << "        vec3 _L = _lv / _d;\n";
            out << "        float _att = 1.0 / (uPointLights.lights[i].attenuation_constant"
                   " + uPointLights.lights[i].attenuation_linear * _d"
                   " + uPointLights.lights[i].attenuation_quadratic * _d * _d);\n";
            out << "        vec3 _lc = uPointLights.lights[i].color * uPointLights.lights[i].intensity * _att;\n";
            out << "        float _sh = ((uPointLights.lights[i].flags & 1u) != 0u) ? pointShadow(uint(i), vWorldPos, _N) : 1.0;\n";
            out << "        _Lo += pbrDirectLight(_albedo, _metallic, _roughness, _F0, _N, _V, _NdotV, _L, _lc) * _sh;\n";
            out << "    }\n";
            out << "    outColor = vec4(kAmbient * _albedo + _Lo + " << em.attr("emissive")
                << ", " << em.attr("opacity") << ");\n";
        }
        else  // Forward Unlit：passthrough（base + emissive）。
        {
            out << "    outColor = vec4(" << em.attr("base_color") << " + " << em.attr("emissive")
                << ", " << em.attr("opacity") << ");\n";
        }

        out << "}\n";
        return out.str();
    }

} // namespace lux::shadergen::glsl
