// =============================================================================
//  MaterialLowering.cpp  —  材质图 -> 纯表达式 ShaderIR（迭代式 worklist DFS）
// -----------------------------------------------------------------------------
//  ShaderGen 客户 A。数据模型 rdesc::MaterialGraph 在 description 层；本文件把它
//  lower 成后端中性 ShaderIR（纯数据），供 engine/ 的 GLSL 后端生成 SPIR-V。
//  算法与 lux::matgraph::lowerToIR 同源（显式堆栈 DFS，深依赖链不爆原生栈），
//  但输出端改为通用 outputs + inputs 槽，shading_model/render_state 带出 LowerResult
//  而【不进 ShaderIR】——它们不是"表达式"。
// =============================================================================

#include <lux/engine/shadergen/material/MaterialLowering.hpp>
#include <lux/engine/description/material_graph/Nodes.hpp>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace lux::shadergen::material
{
    namespace
    {
        namespace rdesc = ::lux::rdesc;

        bool isVector(EValueType t) noexcept
        {
            return t == EValueType::Vec2 || t == EValueType::Vec3 || t == EValueType::Vec4;
        }

        const char* typeName(EValueType t) noexcept
        {
            switch (t)
            {
            case EValueType::Float: return "float";
            case EValueType::Vec2:  return "vec2";
            case EValueType::Vec3:  return "vec3";
            case EValueType::Vec4:  return "vec4";
            }
            return "?";
        }

        EOp mapMathOp(rdesc::EMathOp op) noexcept
        {
            switch (op)
            {
            case rdesc::EMathOp::Mul:       return EOp::Mul;
            case rdesc::EMathOp::Add:       return EOp::Add;
            case rdesc::EMathOp::Sub:       return EOp::Sub;
            case rdesc::EMathOp::Div:       return EOp::Div;
            case rdesc::EMathOp::Lerp:      return EOp::Lerp;
            case rdesc::EMathOp::Saturate:  return EOp::Saturate;
            case rdesc::EMathOp::Dot:       return EOp::Dot;
            case rdesc::EMathOp::Min:       return EOp::Min;
            case rdesc::EMathOp::Max:       return EOp::Max;
            case rdesc::EMathOp::Pow:       return EOp::Pow;
            case rdesc::EMathOp::Step:      return EOp::Step;
            case rdesc::EMathOp::Mod:       return EOp::Mod;
            case rdesc::EMathOp::Cross:     return EOp::Cross;
            case rdesc::EMathOp::Reflect:   return EOp::Reflect;
            case rdesc::EMathOp::OneMinus:  return EOp::OneMinus;
            case rdesc::EMathOp::Abs:       return EOp::Abs;
            case rdesc::EMathOp::Sqrt:      return EOp::Sqrt;
            case rdesc::EMathOp::Floor:     return EOp::Floor;
            case rdesc::EMathOp::Fract:     return EOp::Fract;
            case rdesc::EMathOp::Sin:       return EOp::Sin;
            case rdesc::EMathOp::Cos:       return EOp::Cos;
            case rdesc::EMathOp::Normalize: return EOp::Normalize;
            case rdesc::EMathOp::Length:    return EOp::Length;
            }
            return EOp::Mul;
        }

        bool isUnaryMathOp(rdesc::EMathOp op) noexcept
        {
            switch (op)
            {
            case rdesc::EMathOp::Saturate:
            case rdesc::EMathOp::OneMinus:
            case rdesc::EMathOp::Abs:
            case rdesc::EMathOp::Sqrt:
            case rdesc::EMathOp::Floor:
            case rdesc::EMathOp::Fract:
            case rdesc::EMathOp::Sin:
            case rdesc::EMathOp::Cos:
            case rdesc::EMathOp::Normalize:
            case rdesc::EMathOp::Length:
                return true;
            default:
                return false;
            }
        }

        EValueType mathResultType(rdesc::EMathOp op, EValueType operand) noexcept
        {
            if (op == rdesc::EMathOp::Dot || op == rdesc::EMathOp::Length)
                return EValueType::Float;
            return operand;
        }

        int usedInputCount(const rdesc::Node* n) noexcept
        {
            switch (n->kind())
            {
            case rdesc::EMatNodeKind::Constant:
            case rdesc::EMatNodeKind::Input:
                return 0;
            case rdesc::EMatNodeKind::SampleTexture:
            case rdesc::EMatNodeKind::DecodeNormal:
            case rdesc::EMatNodeKind::Swizzle:
            case rdesc::EMatNodeKind::TbnTransform:
                return 1;
            case rdesc::EMatNodeKind::Math:
                return isUnaryMathOp(static_cast<const rdesc::MathNode*>(n)->op) ? 1 : 2;
            case rdesc::EMatNodeKind::Construct:
                return static_cast<int>(n->inputs().size());
            default:
                return 0;
            }
        }

        // 材质着色输入的 interpolant location，对齐引擎材质 vertex shader 的输出布局
        // （复刻旧 material_graph_glsl emitter 的 declIn：跳号是历史，须逐字一致才能让
        // 将来的 shadergen emitter 产出与旧后端等价的 GLSL）。VertexColor 旧后端未声明，
        // 留 -1 由 emitter/ShellTemplate 决定。
        int32_t materialInputLocation(rdesc::EMaterialInput in) noexcept
        {
            switch (in)
            {
            case rdesc::EMaterialInput::WorldPosition: return 0;
            case rdesc::EMaterialInput::WorldNormal:   return 1;
            case rdesc::EMaterialInput::UV0:           return 3;
            case rdesc::EMaterialInput::WorldTangent:  return 5;
            case rdesc::EMaterialInput::VertexColor:   return -1;
            default:                                   return -1;
            }
        }

        // 迭代式（显式堆栈）拓扑遍历 + SSA 发射。color: 0=white,1=gray,2=black。
        struct Lowerer
        {
            const rdesc::MaterialGraph& g;
            LowerResult&                result;
            ShaderIR&                   ir;
            std::string*                error;
            std::unordered_map<rdesc::node_id, int>            color;
            std::unordered_map<rdesc::node_id, uint32_t>       value_of;
            std::unordered_map<rdesc::EMaterialInput, uint32_t> input_slot_of;
            bool                                               ok = true;

            Lowerer(const rdesc::MaterialGraph& g_, LowerResult& r_, std::string* e_)
                : g(g_), result(r_), ir(r_.ir), error(e_)
            {
            }

            bool fail(std::string m)
            {
                if (error && ok)  // 保留首个错误
                    *error = std::move(m);
                ok = false;
                return false;
            }

            uint32_t push(const ShaderIRValue& v)
            {
                const uint32_t i = static_cast<uint32_t>(ir.values.size());
                ir.values.push_back(v);
                return i;
            }

            uint32_t emitConstant(const float c[4], EValueType t)
            {
                ShaderIRValue v{};
                v.op   = EOp::Constant;
                v.type = t;
                v.constant[0] = c[0];
                v.constant[1] = c[1];
                v.constant[2] = c[2];
                v.constant[3] = c[3];
                return push(v);
            }

            // 把材质着色输入语义映射到（按需新建的）通用 inputs 槽。
            uint32_t inputSlot(rdesc::EMaterialInput in)
            {
                auto it = input_slot_of.find(in);
                if (it != input_slot_of.end())
                    return it->second;
                const uint32_t slot = static_cast<uint32_t>(ir.inputs.size());
                InputSlot s;
                s.name          = rdesc::kMaterialInputs[static_cast<size_t>(in)].glsl_name;
                s.type          = rdesc::inputType(in);
                s.location      = materialInputLocation(in);  // 对齐引擎材质 vert interpolant 布局
                s.interpolation = EInterpolation::Smooth;     // 材质着色输入均为插值量
                ir.inputs.push_back(std::move(s));
                input_slot_of[in] = slot;
                return slot;
            }

            bool validateSource(const rdesc::DataPin& pin)
            {
                const rdesc::Node* src = g.node(pin.source.node);
                if (!src)
                    return fail("dangling connection: source node missing");
                if (pin.source.pin >= src->outputs().size())
                    return fail("connection references an invalid source output pin");
                if (pin.source.pin != 0)
                    return fail("multi-output nodes are not supported yet");
                return true;
            }

            // 解析一个输入引脚 -> SSA 值下标。已连接：读已降级来源值 + 类型检查（含
            // UE 式隐式收窄/标量 splat）；未连接：用引脚默认常量物化 Constant。
            uint32_t operandValue(const rdesc::DataPin& pin)
            {
                if (!ok)
                    return kNoValue;

                if (pin.source.valid())
                {
                    auto it = value_of.find(pin.source.node);
                    if (it == value_of.end())
                    {
                        fail("internal: operand was not lowered before use");
                        return kNoValue;
                    }
                    const uint32_t vidx = it->second;
                    const EValueType produced = ir.values[vidx].type;
                    if (produced == pin.type)
                        return vidx;

                    const int ap = static_cast<int>(produced) + 1;   // source arity
                    const int an = static_cast<int>(pin.type) + 1;   // target arity
                    if (ap > an)
                    {
                        // 大 -> 小向量：取前导分量。
                        ShaderIRValue v{};
                        v.op          = EOp::Swizzle;
                        v.type        = pin.type;
                        v.operands[0] = vidx;
                        v.swizzle[0]  = 0; v.swizzle[1] = 1; v.swizzle[2] = 2; v.swizzle[3] = 3;
                        return push(v);
                    }
                    if (produced == EValueType::Float && an > 1)
                    {
                        // 标量 -> 向量：splat（vecN(x)）。
                        ShaderIRValue v{};
                        v.op   = EOp::Construct;
                        v.type = pin.type;
                        for (int i = 0; i < an; ++i) v.operands[static_cast<size_t>(i)] = vidx;
                        return push(v);
                    }
                    fail(std::string("type mismatch: source produces ") + typeName(produced)
                         + " but pin '" + pin.name + "' expects " + typeName(pin.type)
                         + " — insert a Construct node to widen");
                    return kNoValue;
                }

                return emitConstant(pin.constant, pin.type);
            }

            // 迭代式后序 DFS：降级 root 及其依赖子图，返回 root 值下标。
            uint32_t lower(rdesc::node_id root)
            {
                std::vector<rdesc::node_id> stack;
                stack.push_back(root);

                while (ok && !stack.empty())
                {
                    const rdesc::node_id id = stack.back();
                    const int col = color[id];

                    if (col == 2)
                    {
                        stack.pop_back();
                        continue;
                    }

                    const rdesc::Node* n = g.node(id);
                    if (!n)
                    {
                        fail("internal: referenced node id not found");
                        return kNoValue;
                    }

                    if (col == 0)
                    {
                        color[id] = 1;
                        int used = usedInputCount(n);
                        const int pin_count = static_cast<int>(n->inputs().size());
                        if (used > pin_count)
                            used = pin_count;
                        for (int k = 0; k < used; ++k)
                        {
                            const rdesc::DataPin& pin = n->inputs()[k];
                            if (!pin.source.valid())
                                continue;
                            if (!validateSource(pin))
                                return kNoValue;
                            const int sc = color[pin.source.node];
                            if (sc == 1)
                            {
                                fail("cycle detected in material graph");
                                return kNoValue;
                            }
                            if (sc != 2)
                                stack.push_back(pin.source.node);
                        }
                    }
                    else  // col == 1：子节点已发射 -> 发射本节点
                    {
                        const uint32_t idx = emitNode(n);
                        if (!ok)
                            return kNoValue;
                        value_of[id] = idx;
                        color[id]    = 2;
                        stack.pop_back();
                    }
                }

                auto it = value_of.find(root);
                return it == value_of.end() ? kNoValue : it->second;
            }

            uint32_t emitNode(const rdesc::Node* n)
            {
                switch (n->kind())
                {
                case rdesc::EMatNodeKind::Constant:
                {
                    auto* c = static_cast<const rdesc::ConstantNode*>(n);
                    ShaderIRValue v{};
                    v.op   = EOp::Constant;
                    v.type = c->value_type;
                    for (int k = 0; k < 4; ++k)
                        v.constant[k] = c->value[k];
                    return push(v);
                }
                case rdesc::EMatNodeKind::Input:
                {
                    auto* in = static_cast<const rdesc::InputNode*>(n);
                    ShaderIRValue v{};
                    v.op   = EOp::Input;
                    v.type = rdesc::inputType(in->input);
                    v.slot = inputSlot(in->input);
                    return push(v);
                }
                case rdesc::EMatNodeKind::SampleTexture:
                {
                    auto* s = static_cast<const rdesc::SampleTextureNode*>(n);
                    if (n->inputs().empty())
                    {
                        fail("SampleTexture node has no 'uv' input pin");
                        return kNoValue;
                    }
                    const uint32_t uv = operandValue(n->inputs()[0]);
                    if (!ok)
                        return kNoValue;
                    if (s->texture_slot >= ir.textures.size())
                    {
                        fail("SampleTexture references an undeclared texture slot");
                        return kNoValue;
                    }
                    ShaderIRValue v{};
                    v.op          = EOp::SampleTexture;
                    v.type        = EValueType::Vec4;
                    v.slot        = s->texture_slot;
                    v.operands[0] = uv;
                    return push(v);
                }
                case rdesc::EMatNodeKind::Param:
                {
                    auto* p = static_cast<const rdesc::ParamNode*>(n);
                    if (p->param_slot >= ir.params.size())
                    {
                        fail("Param references an undeclared parameter slot");
                        return kNoValue;
                    }
                    ShaderIRValue v{};
                    v.op   = EOp::Param;
                    v.type = ir.params[p->param_slot].type;  // 声明的参数类型为准
                    v.slot = p->param_slot;
                    return push(v);
                }
                case rdesc::EMatNodeKind::Math:
                    return emitMath(static_cast<const rdesc::MathNode*>(n));
                case rdesc::EMatNodeKind::DecodeNormal:
                {
                    if (n->inputs().empty())
                    {
                        fail("DecodeNormal node has no 'rgb' input pin");
                        return kNoValue;
                    }
                    const uint32_t rgb = operandValue(n->inputs()[0]);
                    if (!ok)
                        return kNoValue;
                    ShaderIRValue v{};
                    v.op          = EOp::DecodeNormal;
                    v.type        = EValueType::Vec3;
                    v.operands[0] = rgb;
                    return push(v);
                }
                case rdesc::EMatNodeKind::Swizzle:
                {
                    if (n->inputs().empty())
                    {
                        fail("Swizzle node has no input pin");
                        return kNoValue;
                    }
                    const uint32_t src = operandValue(n->inputs()[0]);
                    if (!ok)
                        return kNoValue;
                    auto* sw = static_cast<const rdesc::SwizzleNode*>(n);
                    ShaderIRValue v{};
                    v.op          = EOp::Swizzle;
                    v.type        = sw->out_type;
                    v.operands[0] = src;
                    for (int k = 0; k < 4; ++k)
                        v.swizzle[k] = sw->components[k];
                    return push(v);
                }
                case rdesc::EMatNodeKind::TbnTransform:
                {
                    if (n->inputs().empty())
                    {
                        fail("TbnTransform node has no input pin");
                        return kNoValue;
                    }
                    const uint32_t src = operandValue(n->inputs()[0]);
                    if (!ok)
                        return kNoValue;
                    ShaderIRValue v{};
                    v.op          = EOp::TbnNormal;
                    v.type        = EValueType::Vec3;
                    v.operands[0] = src;
                    return push(v);
                }
                case rdesc::EMatNodeKind::Construct:
                {
                    auto* cs = static_cast<const rdesc::ConstructNode*>(n);
                    const size_t cnt = n->inputs().size() > 4 ? 4 : n->inputs().size();
                    ShaderIRValue v{};
                    v.op   = EOp::Construct;
                    v.type = cs->out_type;
                    for (size_t k = 0; k < cnt; ++k)
                    {
                        v.operands[k] = operandValue(n->inputs()[k]);
                        if (!ok)
                            return kNoValue;
                    }
                    return push(v);
                }
                default:
                    fail(std::string("unsupported node kind in lowering: ") + rdesc::toString(n->kind()));
                    return kNoValue;
                }
            }

            uint32_t emitMath(const rdesc::MathNode* m)
            {
                if (m->inputs().size() < 2)
                {
                    fail("Math node is missing input pins");
                    return kNoValue;
                }
                if (m->op == rdesc::EMathOp::Lerp)
                {
                    fail("Lerp requires 3 inputs; the 2-input Math node cannot express it yet");
                    return kNoValue;
                }

                if (isUnaryMathOp(m->op))
                {
                    const uint32_t a = operandValue(m->inputs()[0]);
                    if (!ok)
                        return kNoValue;
                    ShaderIRValue v{};
                    v.op          = mapMathOp(m->op);
                    v.type        = mathResultType(m->op, ir.values[a].type);
                    v.operands[0] = a;
                    return push(v);
                }

                const uint32_t a = operandValue(m->inputs()[0]);
                if (!ok)
                    return kNoValue;
                const uint32_t b = operandValue(m->inputs()[1]);
                if (!ok)
                    return kNoValue;

                const EValueType ta = ir.values[a].type;
                const EValueType tb = ir.values[b].type;

                if ((m->op == rdesc::EMathOp::Dot || m->op == rdesc::EMathOp::Cross) && (ta != tb || !isVector(ta)))
                {
                    fail("Dot/Cross require two vectors of equal type");
                    return kNoValue;
                }
                if (ta != tb)
                {
                    fail(std::string("binary math requires equal operand types (")
                         + typeName(ta) + " vs " + typeName(tb) + "; broadcast not supported yet)");
                    return kNoValue;
                }
                ShaderIRValue v{};
                v.op          = mapMathOp(m->op);
                v.type        = mathResultType(m->op, ta);
                v.operands[0] = a;
                v.operands[1] = b;
                return push(v);
            }

            bool run()
            {
                // 1. 找唯一 OutputSurface。
                const rdesc::Node* output = nullptr;
                for (const auto& [id, np] : g.nodes())
                {
                    if (np->kind() == rdesc::EMatNodeKind::OutputSurface)
                    {
                        if (output)
                            return fail("material graph has more than one OutputSurface node");
                        output = np.get();
                    }
                }
                if (!output)
                    return fail("material graph has no OutputSurface node");

                // 2. shading_model + render_state 带出 LowerResult（不进 ShaderIR）。
                result.shading_model = g.shading_model;
                result.alpha_mode    = g.render_state.alpha_mode;
                result.alpha_cutoff  = g.render_state.alpha_cutoff;
                result.double_sided  = g.render_state.double_sided;

                // 3. 资源槽复制进 ShaderIR。
                for (const auto& t : g.texture_slots)
                    ir.textures.push_back({ t.name });
                for (const auto& p : g.param_slots)
                {
                    ParamSlot s;
                    s.name = p.name;
                    s.type = p.type;
                    for (int k = 0; k < 4; ++k)
                        s.dflt[k] = p.dflt[k];
                    ir.params.push_back(std::move(s));
                }

                // 4. 7 个 surface 属性 -> 命名 outputs（连接 -> 降级子图 + 类型检查；
                //    未连接 -> 覆盖常量则物化，否则 value_id=kNoValue（后端用契约默认））。
                const size_t COUNT = static_cast<size_t>(rdesc::EMaterialAttribute::COUNT);
                ir.outputs.reserve(COUNT);
                for (size_t i = 0; i < COUNT; ++i)
                {
                    const rdesc::MaterialAttributeDesc& adesc = rdesc::kMaterialAttributes[i];
                    Output o;
                    o.name     = adesc.glsl_name;
                    o.type     = adesc.type;
                    o.value_id = kNoValue;
                    for (int k = 0; k < 4; ++k)
                        o.dflt[k] = adesc.dflt[k];  // 默认值自包含于 IR（消费方不回查契约）

                    if (i < output->inputs().size())
                    {
                        const rdesc::DataPin& pin = output->inputs()[i];
                        if (pin.source.valid())
                        {
                            if (!validateSource(pin))
                                return false;
                            lower(pin.source.node);
                            if (!ok)
                                return false;
                            o.value_id = operandValue(pin);  // 读已降级值 + 对属性类型检查
                            if (!ok)
                                return false;
                        }
                        else
                        {
                            const float* d = adesc.dflt;
                            const bool overridden =
                                pin.constant[0] != d[0] || pin.constant[1] != d[1] ||
                                pin.constant[2] != d[2] || pin.constant[3] != d[3];
                            if (overridden)
                            {
                                o.value_id = emitConstant(pin.constant, pin.type);
                                if (!ok)
                                    return false;
                            }
                        }
                    }
                    ir.outputs.push_back(std::move(o));
                }

                ir.fingerprint = ::lux::shadergen::computeFingerprint(ir);

                // 最终 shader 缓存键：表达式指纹 + 外壳参数（shading_model 选 BRDF/GBuffer
                // 编码、alpha 决定 discard）—— 改变发射的 SPIR-V 但不在 IR。double_sided
                // 是纯 PSO 状态、不改 SPIR-V，故不进此键。
                uint64_t cf = ir.fingerprint;
                auto cmix = [&](uint64_t x) noexcept { cf ^= x; cf *= 1099511628211ull; };
                cmix(static_cast<uint32_t>(result.shading_model));
                cmix(static_cast<uint8_t>(result.alpha_mode));
                if (result.alpha_mode == rdesc::EAlphaMode::Mask)
                {
                    uint32_t u;
                    std::memcpy(&u, &result.alpha_cutoff, 4);
                    cmix(u);
                }
                result.combined_fingerprint = cf;
                return ok;
            }
        };
    } // namespace

    lux::cxx::expected<LowerResult, std::string>
    lowerMaterial(const rdesc::MaterialGraph& graph)
    {
        LowerResult out{};
        std::string error;
        Lowerer lowerer(graph, out, &error);
        if (!lowerer.run())
            return lux::cxx::unexpected(error.empty() ? std::string("lowerMaterial failed") : std::move(error));
        return out;
    }

} // namespace lux::shadergen::material
