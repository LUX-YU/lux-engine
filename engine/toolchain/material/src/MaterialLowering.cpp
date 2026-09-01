// =============================================================================
//  MaterialLowering.cpp — material graph -> pure expression ShaderIR
//  (iterative worklist DFS)
// -----------------------------------------------------------------------------
//  ShaderGen's Client A. The data model graph::MaterialGraph lives in the
//  description layer; this file lowers it into a backend-neutral ShaderIR
//  (pure data) that engine/'s GLSL backend then turns into SPIR-V.
//  The algorithm shares its lineage with lux::matgraph::lowerToIR (an
//  explicit-stack DFS, so deep dependency chains don't blow the native
//  stack), but the output side is now generic outputs + inputs slots, and
//  shading_model/render_state are carried out via MaterialIR instead of
//  going into ShaderIR — they aren't "expressions".
// =============================================================================

#include <lux/engine/material/compiler/Lowering.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace lux::material::compiler
{
    using namespace ::lux::shadergen;
    using ShaderValueType = ::lux::shadergen::EValueType;

    namespace
    {
        namespace graph = ::lux::material;

        ShaderValueType mapValueType(graph::EValueType type) noexcept
        {
            switch (type)
            {
            case graph::EValueType::FLOAT: return ShaderValueType::FLOAT;
            case graph::EValueType::VEC2: return ShaderValueType::VEC2;
            case graph::EValueType::VEC3: return ShaderValueType::VEC3;
            case graph::EValueType::VEC4: return ShaderValueType::VEC4;
            }
            return ShaderValueType::FLOAT;
        }

        bool isVector(ShaderValueType t) noexcept
        {
            return t == ShaderValueType::VEC2 || t == ShaderValueType::VEC3 || t == ShaderValueType::VEC4;
        }

        const char* typeName(ShaderValueType t) noexcept
        {
            switch (t)
            {
            case ShaderValueType::FLOAT: return "float";
            case ShaderValueType::VEC2:  return "vec2";
            case ShaderValueType::VEC3:  return "vec3";
            case ShaderValueType::VEC4:  return "vec4";
            }
            return "?";
        }

        EOp mapMathOp(graph::EMathOp op) noexcept
        {
            switch (op)
            {
            case graph::EMathOp::MUL:       return EOp::MUL;
            case graph::EMathOp::ADD:       return EOp::ADD;
            case graph::EMathOp::SUB:       return EOp::SUB;
            case graph::EMathOp::DIV:       return EOp::DIV;
            case graph::EMathOp::LERP:      return EOp::LERP;
            case graph::EMathOp::SATURATE:  return EOp::SATURATE;
            case graph::EMathOp::DOT:       return EOp::DOT;
            case graph::EMathOp::MIN:       return EOp::MIN;
            case graph::EMathOp::MAX:       return EOp::MAX;
            case graph::EMathOp::POW:       return EOp::POW;
            case graph::EMathOp::STEP:      return EOp::STEP;
            case graph::EMathOp::MOD:       return EOp::MOD;
            case graph::EMathOp::CROSS:     return EOp::CROSS;
            case graph::EMathOp::REFLECT:   return EOp::REFLECT;
            case graph::EMathOp::ONE_MINUS:  return EOp::ONE_MINUS;
            case graph::EMathOp::ABS:       return EOp::ABS;
            case graph::EMathOp::SQRT:      return EOp::SQRT;
            case graph::EMathOp::FLOOR:     return EOp::FLOOR;
            case graph::EMathOp::FRACT:     return EOp::FRACT;
            case graph::EMathOp::SIN:       return EOp::SIN;
            case graph::EMathOp::COS:       return EOp::COS;
            case graph::EMathOp::NORMALIZE: return EOp::NORMALIZE;
            case graph::EMathOp::LENGTH:    return EOp::LENGTH;
            }
            return EOp::MUL;
        }

        bool isUnaryMathOp(graph::EMathOp op) noexcept
        {
            switch (op)
            {
            case graph::EMathOp::SATURATE:
            case graph::EMathOp::ONE_MINUS:
            case graph::EMathOp::ABS:
            case graph::EMathOp::SQRT:
            case graph::EMathOp::FLOOR:
            case graph::EMathOp::FRACT:
            case graph::EMathOp::SIN:
            case graph::EMathOp::COS:
            case graph::EMathOp::NORMALIZE:
            case graph::EMathOp::LENGTH:
                return true;
            default:
                return false;
            }
        }

        ShaderValueType mathResultType(graph::EMathOp op, ShaderValueType operand) noexcept
        {
            if (op == graph::EMathOp::DOT || op == graph::EMathOp::LENGTH)
                return ShaderValueType::FLOAT;
            return operand;
        }

        int usedInputCount(const graph::Node* n) noexcept
        {
            switch (n->kind())
            {
            case graph::EMatNodeKind::CONSTANT:
            case graph::EMatNodeKind::INPUT:
                return 0;
            case graph::EMatNodeKind::SAMPLE_TEXTURE:
            case graph::EMatNodeKind::DECODE_NORMAL:
            case graph::EMatNodeKind::SWIZZLE:
            case graph::EMatNodeKind::TBN_TRANSFORM:
                return 1;
            case graph::EMatNodeKind::MATH:
                return isUnaryMathOp(static_cast<const graph::MathNode*>(n)->op) ? 1 : 2;
            case graph::EMatNodeKind::CONSTRUCT:
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
        int32_t materialInputLocation(graph::EMaterialInput in) noexcept
        {
            switch (in)
            {
            case graph::EMaterialInput::WORLD_POSITION: return 0;
            case graph::EMaterialInput::WORLD_NORMAL:   return 1;
            case graph::EMaterialInput::UV0:           return 3;
            case graph::EMaterialInput::WORLD_TANGENT:  return 5;
            case graph::EMaterialInput::VERTEX_COLOR:   return -1;
            default:                                   return -1;
            }
        }

        // Iterative (explicit-stack) topological walk + SSA emission.
        // color: 0=white, 1=gray, 2=black.
        struct Lowerer
        {
            const graph::MaterialGraph& g;
            MaterialIR&                result;
            ShaderIR&                   ir;
            MaterialCompileFailure*     error;
            std::unordered_map<graph::node_id, int>            color;
            std::unordered_map<graph::node_id, uint32_t>       value_of;
            std::unordered_map<graph::EMaterialInput, uint32_t> input_slot_of;
            bool                                               ok = true;

            Lowerer(const graph::MaterialGraph& g_, MaterialIR& r_, MaterialCompileFailure* e_)
                : g(g_), result(r_), ir(r_.shader), error(e_)
            {
            }

            bool fail(
                std::string message,
                EMaterialCompileError code = EMaterialCompileError::LOWERING_FAILURE,
                std::uint64_t node_id = graph::invalid_node,
                std::uint32_t pin_index = graph::invalid_pin
            )
            {
                if (error && ok)  // keep the first error only
                    *error = MaterialCompileFailure{code, std::move(message), node_id, pin_index};
                ok = false;
                return false;
            }

            uint32_t push(const ShaderIRValue& v)
            {
                const uint32_t i = static_cast<uint32_t>(ir.values.size());
                ir.values.push_back(v);
                return i;
            }

            uint32_t emitConstant(const float c[4], ShaderValueType t)
            {
                ShaderIRValue v{};
                v.op   = EOp::CONSTANT;
                v.type = t;
                v.constant[0] = c[0];
                v.constant[1] = c[1];
                v.constant[2] = c[2];
                v.constant[3] = c[3];
                return push(v);
            }

            // Maps a material shading-input semantic onto a generic inputs
            // slot, creating it lazily on first use.
            uint32_t inputSlot(graph::EMaterialInput in)
            {
                auto it = input_slot_of.find(in);
                if (it != input_slot_of.end())
                    return it->second;
                const uint32_t slot = static_cast<uint32_t>(ir.inputs.size());
                InputSlot s;
                s.name          = graph::kMaterialInputs[static_cast<size_t>(in)].name;
                s.type          = mapValueType(graph::inputType(in));
                // Aligned with the engine material vertex interpolant layout.
                s.location = materialInputLocation(in);
                s.interpolation = EInterpolation::SMOOTH;
                ir.inputs.push_back(std::move(s));
                input_slot_of[in] = slot;
                return slot;
            }

            bool validateSource(const graph::DataPin& pin)
            {
                const graph::Node* src = g.node(pin.source.node);
                if (!src)
                    return fail("dangling connection: source node missing", EMaterialCompileError::INVALID_GRAPH,
                                pin.source.node, pin.source.pin);
                if (pin.source.pin >= src->outputs().size())
                    return fail("connection references an invalid source output pin",
                                EMaterialCompileError::INVALID_GRAPH, pin.source.node, pin.source.pin);
                if (pin.source.pin != 0)
                    return fail("multi-output nodes are not supported yet");
                return true;
            }

            // Resolves an input pin -> SSA value index. If connected: reads
            // the already-lowered source value and type-checks it (including
            // UE-style implicit narrowing / scalar splat); if unconnected:
            // materializes a Constant from the pin's default value.
            uint32_t operandValue(const graph::DataPin& pin)
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
                    const ShaderValueType produced = ir.values[vidx].type;
                    if (produced == mapValueType(pin.type))
                        return vidx;

                    const int ap = static_cast<int>(produced) + 1;   // source arity
                    const int an = static_cast<int>(pin.type) + 1;   // target arity
                    if (ap > an)
                    {
                        // larger -> smaller vector: take the leading components.
                        ShaderIRValue v{};
                        v.op          = EOp::SWIZZLE;
                        v.type        = mapValueType(pin.type);
                        v.operands[0] = vidx;
                        v.swizzle[0]  = 0; v.swizzle[1] = 1; v.swizzle[2] = 2; v.swizzle[3] = 3;
                        return push(v);
                    }
                    if (produced == ShaderValueType::FLOAT && an > 1)
                    {
                        // scalar -> vector: splat (vecN(x)).
                        ShaderIRValue v{};
                        v.op   = EOp::CONSTRUCT;
                        v.type = mapValueType(pin.type);
                        for (int i = 0; i < an; ++i) v.operands[static_cast<size_t>(i)] = vidx;
                        return push(v);
                    }
                    fail(std::string("type mismatch: source produces ") + typeName(produced) + " but pin '" +
                             pin.name + "' expects " + typeName(mapValueType(pin.type)) +
                             " — insert a Construct node to widen",
                         EMaterialCompileError::TYPE_MISMATCH, pin.source.node, pin.source.pin);
                    return kNoValue;
                }

                return emitConstant(pin.constant, mapValueType(pin.type));
            }

            // Iterative post-order DFS: lowers root and its dependency
            // subgraph, returning root's value index.
            uint32_t lower(graph::node_id root)
            {
                std::vector<graph::node_id> stack;
                stack.push_back(root);

                while (ok && !stack.empty())
                {
                    const graph::node_id id = stack.back();
                    const int col = color[id];

                    if (col == 2)
                    {
                        stack.pop_back();
                        continue;
                    }

                    const graph::Node* n = g.node(id);
                    if (!n)
                    {
                        fail("referenced node id not found", EMaterialCompileError::INVALID_GRAPH, id);
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
                            const graph::DataPin& pin = n->inputs()[k];
                            if (!pin.source.valid())
                                continue;
                            if (!validateSource(pin))
                                return kNoValue;
                            const int sc = color[pin.source.node];
                            if (sc == 1)
                            {
                                fail("cycle detected in material graph", EMaterialCompileError::CYCLE,
                                     pin.source.node);
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

            uint32_t emitNode(const graph::Node* n)
            {
                switch (n->kind())
                {
                case graph::EMatNodeKind::CONSTANT:
                {
                    auto* c = static_cast<const graph::ConstantNode*>(n);
                    ShaderIRValue v{};
                    v.op   = EOp::CONSTANT;
                    v.type = mapValueType(c->value_type);
                    for (int k = 0; k < 4; ++k)
                        v.constant[k] = c->value[k];
                    return push(v);
                }
                case graph::EMatNodeKind::INPUT:
                {
                    auto* in = static_cast<const graph::InputNode*>(n);
                    ShaderIRValue v{};
                    v.op   = EOp::INPUT;
                    v.type = mapValueType(graph::inputType(in->input));
                    v.slot = inputSlot(in->input);
                    return push(v);
                }
                case graph::EMatNodeKind::SAMPLE_TEXTURE:
                {
                    auto* s = static_cast<const graph::SampleTextureNode*>(n);
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
                        fail("SampleTexture references an undeclared texture slot",
                             EMaterialCompileError::INVALID_GRAPH, n->id());
                        return kNoValue;
                    }
                    ShaderIRValue v{};
                    v.op          = EOp::SAMPLE_TEXTURE;
                    v.type        = ShaderValueType::VEC4;
                    v.slot        = s->texture_slot;
                    v.operands[0] = uv;
                    return push(v);
                }
                case graph::EMatNodeKind::PARAM:
                {
                    auto* p = static_cast<const graph::ParamNode*>(n);
                    if (p->param_slot >= ir.params.size())
                    {
                        fail("Param references an undeclared parameter slot",
                             EMaterialCompileError::INVALID_GRAPH, n->id());
                        return kNoValue;
                    }
                    ShaderIRValue v{};
                    v.op   = EOp::PARAM;
                    v.type = ir.params[p->param_slot].type;  // authoritative: the declared parameter type
                    v.slot = p->param_slot;
                    return push(v);
                }
                case graph::EMatNodeKind::MATH:
                    return emitMath(static_cast<const graph::MathNode*>(n));
                case graph::EMatNodeKind::DECODE_NORMAL:
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
                    v.op          = EOp::DECODE_NORMAL;
                    v.type        = ShaderValueType::VEC3;
                    v.operands[0] = rgb;
                    return push(v);
                }
                case graph::EMatNodeKind::SWIZZLE:
                {
                    if (n->inputs().empty())
                    {
                        fail("Swizzle node has no input pin");
                        return kNoValue;
                    }
                    const uint32_t src = operandValue(n->inputs()[0]);
                    if (!ok)
                        return kNoValue;
                    auto* sw = static_cast<const graph::SwizzleNode*>(n);
                    ShaderIRValue v{};
                    v.op          = EOp::SWIZZLE;
                    v.type        = mapValueType(sw->out_type);
                    v.operands[0] = src;
                    for (int k = 0; k < 4; ++k)
                        v.swizzle[k] = sw->components[k];
                    return push(v);
                }
                case graph::EMatNodeKind::TBN_TRANSFORM:
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
                    v.op          = EOp::TBN_NORMAL;
                    v.type        = ShaderValueType::VEC3;
                    v.operands[0] = src;
                    return push(v);
                }
                case graph::EMatNodeKind::CONSTRUCT:
                {
                    auto* cs = static_cast<const graph::ConstructNode*>(n);
                    const size_t cnt = n->inputs().size() > 4 ? 4 : n->inputs().size();
                    ShaderIRValue v{};
                    v.op   = EOp::CONSTRUCT;
                    v.type = mapValueType(cs->out_type);
                    for (size_t k = 0; k < cnt; ++k)
                    {
                        v.operands[k] = operandValue(n->inputs()[k]);
                        if (!ok)
                            return kNoValue;
                    }
                    return push(v);
                }
                default:
                    fail(std::string("unsupported node kind in lowering: ") + graph::toString(n->kind()));
                    return kNoValue;
                }
            }

            uint32_t emitMath(const graph::MathNode* m)
            {
                if (m->inputs().size() < 2)
                {
                    fail("Math node is missing input pins");
                    return kNoValue;
                }
                if (m->op == graph::EMathOp::LERP)
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

                const ShaderValueType ta = ir.values[a].type;
                const ShaderValueType tb = ir.values[b].type;

                if ((m->op == graph::EMathOp::DOT || m->op == graph::EMathOp::CROSS) && (ta != tb || !isVector(ta)))
                {
                    fail("Dot/Cross require two vectors of equal type", EMaterialCompileError::TYPE_MISMATCH,
                         m->id());
                    return kNoValue;
                }
                if (ta != tb)
                {
                    fail(std::string("binary math requires equal operand types (") + typeName(ta) + " vs " +
                             typeName(tb) + "; broadcast not supported yet)",
                         EMaterialCompileError::TYPE_MISMATCH, m->id());
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
                const graph::Node* output = nullptr;
                for (const auto& [id, np] : g.nodes())
                {
                    if (np->kind() == graph::EMatNodeKind::OUTPUT_SURFACE)
                    {
                        if (output)
                            return fail("material graph has more than one OutputSurface node",
                                        EMaterialCompileError::INVALID_GRAPH, id);
                        output = np.get();
                    }
                }
                if (!output)
                    return fail("material graph has no OutputSurface node",
                                EMaterialCompileError::MISSING_REQUIRED_OUTPUT);

                // 2. Carry shading_model + render_state out via MaterialIR
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
                    s.type = mapValueType(p.type);
                    for (int k = 0; k < 4; ++k)
                        s.dflt[k] = p.dflt[k];
                    ir.params.push_back(std::move(s));
                }

                // 4. 7 surface attributes -> named outputs (connected -> lower
                //    the subgraph + type-check; unconnected -> materialize if
                //    the constant was overridden, otherwise value_id=kNoValue
                //    so the backend falls back to the contract default).
                const size_t COUNT = static_cast<size_t>(graph::EMaterialAttribute::COUNT);
                ir.outputs.reserve(COUNT);
                for (size_t i = 0; i < COUNT; ++i)
                {
                    const graph::MaterialAttributeDesc& adesc = graph::kMaterialAttributes[i];
                    Output o;
                    o.name     = adesc.name;
                    o.type     = mapValueType(adesc.type);
                    o.value_id = kNoValue;
                    for (int k = 0; k < 4; ++k)
                        o.dflt[k] = adesc.dflt[k];

                    if (i < output->inputs().size())
                    {
                        const graph::DataPin& pin = output->inputs()[i];
                        if (pin.source.valid())
                        {
                            if (!validateSource(pin))
                                return false;
                            lower(pin.source.node);
                            if (!ok)
                                return false;
                            o.value_id = operandValue(pin);
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
                                o.value_id = emitConstant(pin.constant, mapValueType(pin.type));
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
                if (result.alpha_mode == ::lux::rdesc::EAlphaMode::Mask)
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

    lux::cxx::expected<MaterialIR, MaterialCompileFailure>
    lowerMaterial(const graph::MaterialGraph& graph)
    {
        MaterialIR out{};
        MaterialCompileFailure error{
            EMaterialCompileError::LOWERING_FAILURE,
            "lowerMaterial failed",
            graph::invalid_node,
            graph::invalid_pin
        };
        Lowerer lowerer(graph, out, &error);
        if (!lowerer.run())
            return lux::cxx::unexpected(std::move(error));
        return out;
    }

} // namespace lux::material::compiler
