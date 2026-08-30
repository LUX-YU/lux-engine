// =============================================================================
//  MaterialLowering.cpp — material graph -> pure expression ShaderIR
//  (iterative worklist DFS)
// -----------------------------------------------------------------------------
//  ShaderGen's Client A. The data model rdesc::MaterialGraph lives in the
//  description layer; this file lowers it into a backend-neutral ShaderIR
//  (pure data) that engine/'s GLSL backend then turns into SPIR-V.
//  The algorithm shares its lineage with lux::matgraph::lowerToIR (an
//  explicit-stack DFS, so deep dependency chains don't blow the native
//  stack), but the output side is now generic outputs + inputs slots, and
//  shading_model/render_state are carried out via LowerResult instead of
//  going into ShaderIR — they aren't "expressions".
// =============================================================================

#include <lux/engine/toolchain/asset/material/MaterialLowering.hpp>
#include <lux/engine/authoring/material/Nodes.hpp>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace lux::shadergen::material
{
    namespace
    {
        namespace rdesc = ::lux::authoring::material;

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

        // Interpolant location for material shading inputs, aligned with the
        // engine material vertex shader's output layout (mirrors the old
        // material_graph_glsl emitter's declIn: the gaps in numbering are
        // historical, and must match exactly so the shadergen emitter's
        // output stays equivalent to the old backend's GLSL). VertexColor was
        // never declared by the old backend, so it's left at -1 for the
        // emitter/ShellTemplate to decide.
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

        // Iterative (explicit-stack) topological walk + SSA emission.
        // color: 0=white, 1=gray, 2=black.
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
                if (error && ok)  // keep the first error only
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

            // Maps a material shading-input semantic onto a generic inputs
            // slot, creating it lazily on first use.
            uint32_t inputSlot(rdesc::EMaterialInput in)
            {
                auto it = input_slot_of.find(in);
                if (it != input_slot_of.end())
                    return it->second;
                const uint32_t slot = static_cast<uint32_t>(ir.inputs.size());
                InputSlot s;
                s.name          = rdesc::kMaterialInputs[static_cast<size_t>(in)].glsl_name;
                s.type          = rdesc::inputType(in);
                s.location      = materialInputLocation(in);  // aligned with the engine material vert interpolant layout
                s.interpolation = EInterpolation::Smooth;     // material shading inputs are always interpolated
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

            // Resolves an input pin -> SSA value index. If connected: reads
            // the already-lowered source value and type-checks it (including
            // UE-style implicit narrowing / scalar splat); if unconnected:
            // materializes a Constant from the pin's default value.
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
                        // larger -> smaller vector: take the leading components.
                        ShaderIRValue v{};
                        v.op          = EOp::Swizzle;
                        v.type        = pin.type;
                        v.operands[0] = vidx;
                        v.swizzle[0]  = 0; v.swizzle[1] = 1; v.swizzle[2] = 2; v.swizzle[3] = 3;
                        return push(v);
                    }
                    if (produced == EValueType::Float && an > 1)
                    {
                        // scalar -> vector: splat (vecN(x)).
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

            // Iterative post-order DFS: lowers root and its dependency
            // subgraph, returning root's value index.
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
                    else  // col == 1: children already emitted -> emit this node
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
                    v.type = ir.params[p->param_slot].type;  // authoritative: the declared parameter type
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
                // 1. Find the single OutputSurface.
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

                // 2. Carry shading_model + render_state out via LowerResult
                //    (they do not go into ShaderIR).
                result.shading_model = g.shading_model;
                result.alpha_mode    = g.render_state.alpha_mode;
                result.alpha_cutoff  = g.render_state.alpha_cutoff;
                result.double_sided  = g.render_state.double_sided;

                // 3. Copy resource slots into ShaderIR.
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

                // 4. 7 surface attributes -> named outputs (connected -> lower
                //    the subgraph + type-check; unconnected -> materialize if
                //    the constant was overridden, otherwise value_id=kNoValue
                //    so the backend falls back to the contract default).
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
                        o.dflt[k] = adesc.dflt[k];  // default value is self-contained in the IR (consumers never look back at the contract)

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
                            o.value_id = operandValue(pin);  // reads the already-lowered value + type-checks against the attribute
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

                // Final shader cache key: the expression fingerprint combined
                // with the "shell" parameters (shading_model selects the
                // BRDF/GBuffer encoding, alpha decides whether to discard) —
                // these change the emitted SPIR-V but aren't part of the IR.
                // double_sided is pure PSO state that doesn't change the
                // SPIR-V, so it does not enter this key.
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
