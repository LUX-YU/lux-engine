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

#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/detail/BuiltinMaterialNodes.hpp>
#include <lux/engine/material/detail/MaterialMath.hpp>
#include <lux/engine/material/detail/MaterialValidation.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace lux::material
{
    using namespace ::lux::shadergen;
    using ShaderValueType = ::lux::shadergen::EValueType;

    namespace
    {
        namespace graph = ::lux::material;

        bool mapValueType(graph::EValueType type, ShaderValueType& result) noexcept
        {
            switch (type)
            {
            case graph::EValueType::FLOAT:
                result = ShaderValueType::FLOAT;
                return true;
            case graph::EValueType::VEC2:
                result = ShaderValueType::VEC2;
                return true;
            case graph::EValueType::VEC3:
                result = ShaderValueType::VEC3;
                return true;
            case graph::EValueType::VEC4:
                result = ShaderValueType::VEC4;
                return true;
            }
            return false;
        }

        const char* typeName(ShaderValueType t) noexcept
        {
            switch (t)
            {
            case ShaderValueType::FLOAT:
                return "float";
            case ShaderValueType::VEC2:
                return "vec2";
            case ShaderValueType::VEC3:
                return "vec3";
            case ShaderValueType::VEC4:
                return "vec4";
            }
            return "?";
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
                return static_cast<int>(detail::mathInputCount(static_cast<const graph::MathNode*>(n)->op));
            case graph::EMatNodeKind::CONSTRUCT:
                return static_cast<int>(n->inputs().size());
            default:
                return 0;
            }
        }

        // Iterative (explicit-stack) topological walk + SSA emission.
        // color: 0=white, 1=gray, 2=black.
        struct Lowerer
        {
            const graph::MaterialGraph& g;
            MaterialIR& result;
            ShaderIR& ir;
            MaterialCompileFailure* error;
            std::unordered_map<graph::NodeId, int> color;
            std::unordered_map<graph::NodeId, uint32_t> value_of;
            bool ok = true;

            Lowerer(const graph::MaterialGraph& g_, MaterialIR& r_, MaterialCompileFailure* e_)
                : g(g_), result(r_), ir(r_.shader), error(e_)
            {
            }

            bool fail(
                std::string message,
                EMaterialCompileError code = EMaterialCompileError::LOWERING_FAILURE,
                graph::NodeId node_id = {},
                std::uint32_t pin_index = graph::invalid_pin
            )
            {
                if (error && ok) // keep the first error only
                {
                    *error = MaterialCompileFailure{code, std::move(message), node_id, pin_index};
                }
                ok = false;
                return false;
            }

            ShaderValueType valueType(
                graph::EValueType source,
                graph::NodeId node_id = {},
                std::uint32_t pin_index = graph::invalid_pin
            )
            {
                ShaderValueType result{};
                if (!mapValueType(source, result))
                {
                    fail(
                        "invalid material value type reached lowering",
                        EMaterialCompileError::INVALID_GRAPH,
                        node_id,
                        pin_index
                    );
                }
                return result;
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
                v.op = EOp::CONSTANT;
                v.type = t;
                v.constant[0] = c[0];
                v.constant[1] = c[1];
                v.constant[2] = c[2];
                v.constant[3] = c[3];
                return push(v);
            }

            bool validateSource(graph::PinLink source)
            {
                const graph::Node* src = g.node(source.node);
                if (!src)
                {
                    return fail(
                        "dangling connection: source node missing",
                        EMaterialCompileError::INVALID_GRAPH,
                        source.node,
                        source.pin
                    );
                }
                if (source.pin >= src->outputs().size())
                {
                    return fail(
                        "connection references an invalid source output pin",
                        EMaterialCompileError::INVALID_GRAPH,
                        source.node,
                        source.pin
                    );
                }
                if (source.pin != 0)
                {
                    return fail("multi-output nodes are not supported yet");
                }
                return true;
            }

            // Resolves an input pin -> SSA value index. If connected: reads
            // the already-lowered source value and type-checks it (including
            // UE-style implicit narrowing / scalar splat); if unconnected:
            // materializes a Constant from the pin's default value.
            uint32_t operandValue(const graph::DataPin& pin)
            {
                if (!ok)
                {
                    return kNoValue;
                }

                const auto source = g.source(pin.id);
                if (source.valid())
                {
                    auto it = value_of.find(source.node);
                    if (it == value_of.end())
                    {
                        fail("internal: operand was not lowered before use");
                        return kNoValue;
                    }
                    const uint32_t vidx = it->second;
                    const ShaderValueType produced = ir.values[vidx].type;
                    const auto expected = valueType(pin.type, source.node, source.pin);
                    if (!ok)
                    {
                        return kNoValue;
                    }
                    if (produced == expected)
                    {
                        return vidx;
                    }

                    const int ap = static_cast<int>(produced) + 1; // source arity
                    const int an = static_cast<int>(pin.type) + 1; // target arity
                    if (ap > an)
                    {
                        // larger -> smaller vector: take the leading components.
                        ShaderIRValue v{};
                        v.op = EOp::SWIZZLE;
                        v.type = expected;
                        v.operands[0] = vidx;
                        v.swizzle[0] = 0;
                        v.swizzle[1] = 1;
                        v.swizzle[2] = 2;
                        v.swizzle[3] = 3;
                        return push(v);
                    }
                    if (produced == ShaderValueType::FLOAT && an > 1)
                    {
                        // scalar -> vector: splat (vecN(x)).
                        ShaderIRValue v{};
                        v.op = EOp::CONSTRUCT;
                        v.type = expected;
                        for (int i = 0; i < an; ++i)
                        {
                            v.operands[static_cast<size_t>(i)] = vidx;
                        }
                        return push(v);
                    }
                    fail(
                        std::string("type mismatch: source produces ") + typeName(produced) + " but pin '" + pin.name +
                            "' expects " + typeName(expected) + " — insert a Construct node to widen",
                        EMaterialCompileError::TYPE_MISMATCH,
                        source.node,
                        source.pin
                    );
                    return kNoValue;
                }

                const auto type = valueType(pin.type);
                return ok ? emitConstant(pin.constant, type) : kNoValue;
            }

            // Iterative post-order DFS: lowers root and its dependency
            // subgraph, returning root's value index.
            uint32_t lower(graph::NodeId root)
            {
                std::vector<graph::NodeId> stack;
                stack.push_back(root);

                while (ok && !stack.empty())
                {
                    const graph::NodeId id = stack.back();
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
                        {
                            used = pin_count;
                        }
                        for (int k = 0; k < used; ++k)
                        {
                            const graph::DataPin& pin = n->inputs()[k];
                            const auto source = g.source(pin.id);
                            if (!source.valid())
                            {
                                continue;
                            }
                            if (!validateSource(source))
                            {
                                return kNoValue;
                            }
                            const int sc = color[source.node];
                            if (sc == 1)
                            {
                                fail("cycle detected in material graph", EMaterialCompileError::CYCLE, source.node);
                                return kNoValue;
                            }
                            if (sc != 2)
                            {
                                stack.push_back(source.node);
                            }
                        }
                    }
                    else // col == 1: children already emitted -> emit this node
                    {
                        const uint32_t idx = emitNode(id, n);
                        if (!ok)
                        {
                            return kNoValue;
                        }
                        value_of[id] = idx;
                        color[id] = 2;
                        stack.pop_back();
                    }
                }

                auto it = value_of.find(root);
                return it == value_of.end() ? kNoValue : it->second;
            }

            template <class T> uint32_t emitBuiltin(graph::NodeId id, const graph::Node& node, const T& payload)
            {
                std::array<std::uint32_t, 4> inputs{};
                for (std::size_t index = 0; index != node.inputs().size(); ++index)
                {
                    inputs[index] = operandValue(node.inputs()[index]);
                    if (!ok)
                    {
                        return kNoValue;
                    }
                }
                auto emitted = detail::appendBuiltin(payload, {inputs.data(), node.inputs().size()}, ir);
                if (!emitted)
                {
                    auto failure = std::move(emitted.error());
                    fail(std::move(failure.message), failure.code, id, failure.pin_index);
                    return kNoValue;
                }
                return *emitted;
            }

            uint32_t emitNode(graph::NodeId id, const graph::Node* n)
            {
                // Transitional storage projection only: the registered semantic emitters below own
                // compilation. The old Node representation is removed with the graph-store migration.
                switch (n->kind())
                {
                case graph::EMatNodeKind::CONSTANT:
                {
                    const auto& value = static_cast<const graph::ConstantNode&>(*n);
                    const MaterialConstant payload{
                        {value.value[0], value.value[1], value.value[2], value.value[3]},
                        value.value_type
                    };
                    return emitBuiltin(id, *n, payload);
                }
                case graph::EMatNodeKind::INPUT:
                    return emitBuiltin(id, *n, MaterialInput{static_cast<const graph::InputNode&>(*n).input});
                case graph::EMatNodeKind::SAMPLE_TEXTURE:
                    return emitBuiltin(
                        id,
                        *n,
                        MaterialSampleTexture{static_cast<const graph::SampleTextureNode&>(*n).texture_slot}
                    );
                case graph::EMatNodeKind::PARAM:
                {
                    const auto& value = static_cast<const graph::ParamNode&>(*n);
                    return emitBuiltin(id, *n, MaterialParameter{value.param_slot, value.type});
                }
                case graph::EMatNodeKind::MATH:
                    return emitMath(id, static_cast<const graph::MathNode*>(n));
                case graph::EMatNodeKind::DECODE_NORMAL:
                    return emitBuiltin(id, *n, MaterialDecodeNormal{});
                case graph::EMatNodeKind::SWIZZLE:
                {
                    const auto& value = static_cast<const graph::SwizzleNode&>(*n);
                    const MaterialSwizzle payload{
                        value.source_type,
                        value.out_type,
                        {value.components[0], value.components[1], value.components[2], value.components[3]}
                    };
                    return emitBuiltin(id, *n, payload);
                }
                case graph::EMatNodeKind::TBN_TRANSFORM:
                    return emitBuiltin(id, *n, MaterialTbnTransform{});
                case graph::EMatNodeKind::CONSTRUCT:
                    return emitBuiltin(
                        id,
                        *n,
                        MaterialConstruct{static_cast<const graph::ConstructNode&>(*n).out_type}
                    );
                default:
                    fail(std::string("unsupported node kind in lowering: ") + graph::toString(n->kind()));
                    return kNoValue;
                }
            }

            uint32_t emitMath(graph::NodeId id, const graph::MathNode* node)
            {
                const MaterialMath math{node->op, node->operand_type};
                std::array<std::uint32_t, 2> inputs{kNoValue, kNoValue};
                for (std::size_t index = 0; index != detail::mathInputCount(math.op); ++index)
                {
                    inputs[index] = operandValue(node->inputs()[index]);
                    if (!ok)
                    {
                        return kNoValue;
                    }
                }
                auto output = detail::appendMath(math, inputs, ir);
                if (!output)
                {
                    auto failure = std::move(output.error());
                    fail(std::move(failure.message), failure.code, id, failure.pin_index);
                    return kNoValue;
                }
                return *output;
            }

            bool run()
            {
                // 1. Find the single OutputSurface.
                const graph::Node* output = nullptr;
                graph::NodeId output_id;
                for (const auto& [id, np] : g.nodes())
                {
                    if (np->kind() == graph::EMatNodeKind::OUTPUT_SURFACE)
                    {
                        if (output)
                        {
                            return fail(
                                "material graph has more than one OutputSurface node",
                                EMaterialCompileError::INVALID_GRAPH,
                                id
                            );
                        }
                        output = np;
                        output_id = id;
                    }
                }
                if (!output)
                {
                    return fail(
                        "material graph has no OutputSurface node",
                        EMaterialCompileError::MISSING_REQUIRED_OUTPUT
                    );
                }

                // 2. Carry shading_model + render_state out via MaterialIR
                //    (they do not go into ShaderIR).
                result.shading_model = g.shading_model;
                result.alpha_mode = g.render_state.alpha_mode;
                result.alpha_cutoff = g.render_state.alpha_cutoff;
                result.double_sided = g.render_state.double_sided;

                // 3. Copy resource slots into ShaderIR.
                for (const auto& t : g.texture_slots)
                {
                    ir.textures.push_back({t.name});
                }
                for (const auto& p : g.param_slots)
                {
                    ParamSlot s;
                    s.name = p.name;
                    s.type = valueType(p.type);
                    if (!ok)
                    {
                        return false;
                    }
                    for (int k = 0; k < 4; ++k)
                    {
                        s.dflt[k] = p.dflt[k];
                    }
                    ir.params.push_back(std::move(s));
                }

                // 4. 7 surface attributes -> named outputs (connected -> lower
                //    the subgraph + type-check; unconnected -> materialize if
                //    the constant was overridden, otherwise value_id=kNoValue
                //    so the backend falls back to the contract default).
                const size_t COUNT = static_cast<size_t>(graph::EMaterialAttribute::COUNT);
                std::array<std::uint32_t, COUNT> surface_inputs;
                surface_inputs.fill(kNoValue);
                for (size_t i = 0; i < COUNT; ++i)
                {
                    const graph::MaterialAttributeDesc& adesc = graph::kMaterialAttributes[i];
                    if (i < output->inputs().size())
                    {
                        const graph::DataPin& pin = output->inputs()[i];
                        const auto source = g.source(pin.id);
                        if (source.valid())
                        {
                            if (!validateSource(source))
                            {
                                return false;
                            }
                            lower(source.node);
                            if (!ok)
                            {
                                return false;
                            }
                            surface_inputs[i] = operandValue(pin);
                            if (!ok)
                            {
                                return false;
                            }
                        }
                        else
                        {
                            const float* d = adesc.dflt;
                            const bool overridden = pin.constant[0] != d[0] || pin.constant[1] != d[1] ||
                                                    pin.constant[2] != d[2] || pin.constant[3] != d[3];
                            if (overridden)
                            {
                                const auto type = valueType(pin.type, output_id, static_cast<std::uint32_t>(i));
                                if (!ok)
                                {
                                    return false;
                                }
                                surface_inputs[i] = emitConstant(pin.constant, type);
                                if (!ok)
                                {
                                    return false;
                                }
                            }
                        }
                    }
                }
                auto surface = detail::appendSurface(surface_inputs, ir);
                if (!surface)
                {
                    auto failure = std::move(surface.error());
                    return fail(std::move(failure.message), failure.code, output_id, failure.pin_index);
                }

                ir.fingerprint = ::lux::shadergen::computeFingerprint(ir);

                // Final shader cache key: the expression fingerprint combined
                // with the "shell" parameters (shading_model selects the
                // BRDF/GBuffer encoding, alpha decides whether to discard) —
                // these change the emitted SPIR-V but aren't part of the IR.
                // double_sided is pure PSO state that doesn't change the
                // SPIR-V, so it does not enter this key.
                uint64_t cf = ir.fingerprint;
                auto cmix = [&](uint64_t x) noexcept
                {
                    cf ^= x;
                    cf *= 1099511628211ull;
                };
                cmix(static_cast<uint32_t>(result.shading_model));
                cmix(static_cast<uint8_t>(result.alpha_mode));
                if (result.alpha_mode == ::lux::rdesc::EAlphaMode::MASK)
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

    lux::cxx::expected<MaterialIR, MaterialCompileFailure> lowerMaterial(const graph::MaterialGraph& graph) noexcept
    {
        auto validation = detail::validateMaterialGraph(graph);
        if (!validation)
        {
            return lux::cxx::unexpected(std::move(validation.error()));
        }
        MaterialIR out{};
        MaterialCompileFailure
            error{EMaterialCompileError::LOWERING_FAILURE, "lowerMaterial failed", {}, graph::invalid_pin};
        Lowerer lowerer(graph, out, &error);
        if (!lowerer.run())
        {
            return lux::cxx::unexpected(std::move(error));
        }
        return out;
    }

} // namespace lux::material
