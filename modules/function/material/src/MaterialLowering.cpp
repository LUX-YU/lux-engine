#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/detail/MaterialValidation.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>

#include <algorithm>
#include <bit>
#include <unordered_map>
#include <vector>

namespace lux::material
{
    namespace
    {
        using shadergen::EOp;
        using shadergen::kNoValue;
        using shadergen::ShaderIR;
        using shadergen::ShaderIRValue;
        using ShaderType = shadergen::EValueType;

        const char* typeName(ShaderType type) noexcept
        {
            constexpr const char* Names[]{"float", "vec2", "vec3", "vec4"};
            return Names[static_cast<std::size_t>(type)];
        }

        // Per-compilation indexes borrow the frozen graph. Node payloads never own topology or SSA state.
        struct Lowerer final
        {
            const MaterialGraph& graph;
            const detail::MaterialCompileGraph& description;
            ShaderIR& ir;
            MaterialCompileFailure error;
            std::unordered_map<NodeId, unsigned> color;
            std::unordered_map<PinId, std::uint32_t> values;
            bool ok{true};

            bool fail(MaterialCompileFailure failure, NodeId node = {}) noexcept
            {
                if (ok)
                {
                    if (node.valid())
                    {
                        failure.node_id = node;
                    }
                    error = std::move(failure);
                }
                ok = false;
                return false;
            }

            std::uint32_t append(ShaderIRValue value) noexcept
            {
                const auto index = static_cast<std::uint32_t>(ir.values.size());
                ir.values.push_back(value);
                return index;
            }

            PinId source(PinId input) const noexcept
            {
                const auto found = description.incoming.find(input);
                return found == description.incoming.end() ? PinId{} : found->second;
            }

            std::uint32_t constant(const MaterialPinPayload& pin) noexcept
            {
                ShaderIRValue value;
                value.op = EOp::CONSTANT;
                value.type = static_cast<ShaderType>(pin.type);
                std::copy(pin.constant.begin(), pin.constant.end(), value.constant);
                return append(value);
            }

            std::uint32_t operand(const detail::MaterialCompilePin& input) noexcept
            {
                if (input.declaration.input_use == EMaterialInputUse::UNUSED)
                {
                    return kNoValue;
                }
                const auto& pin = *graph.pin(input.id);
                const auto output = source(input.id);
                if (!output.valid())
                {
                    const bool is_optional = input.declaration.input_use == EMaterialInputUse::CONNECTED_VALUE;
                    const bool is_default = pin.constant == input.declaration.default_value;
                    return is_optional && is_default ? kNoValue : constant(pin);
                }
                const auto found = values.find(output);
                if (found == values.end())
                {
                    fail({EMaterialCompileError::LOWERING_FAILURE, "internal: operand was not lowered before use"});
                    return kNoValue;
                }
                const auto index = found->second;
                const auto produced = ir.values[index].type;
                const auto expected = static_cast<ShaderType>(pin.type);
                if (produced == expected)
                {
                    return index;
                }
                const auto source_arity = static_cast<unsigned>(produced) + 1;
                const auto target_arity = static_cast<unsigned>(expected) + 1;
                ShaderIRValue value;
                value.type = expected;
                if (source_arity > target_arity)
                {
                    value.op = EOp::SWIZZLE;
                    value.operands[0] = index;
                    for (unsigned component = 0; component != 4; ++component)
                    {
                        value.swizzle[component] = static_cast<std::uint8_t>(component);
                    }
                    return append(value);
                }
                if (produced == ShaderType::FLOAT)
                {
                    value.op = EOp::CONSTRUCT;
                    for (unsigned component = 0; component != target_arity; ++component)
                    {
                        value.operands[component] = index;
                    }
                    return append(value);
                }
                const auto owner = description.pins.at(output)->owner;
                const auto& outputs = description.nodes.at(owner).outputs;
                const auto ordinal = static_cast<std::uint32_t>(std::ranges::find(outputs, output) - outputs.begin());
                fail(
                    {EMaterialCompileError::TYPE_MISMATCH,
                     std::string("type mismatch: source produces ") + typeName(produced) + " but pin '" + pin.name +
                         "' expects " + typeName(expected) + " — insert a Construct node to widen",
                     owner,
                     ordinal}
                );
                return kNoValue;
            }

            bool compile(NodeId id, std::span<const std::uint32_t> inputs) noexcept
            {
                const auto& schema = description.nodes.at(id);
                const auto& node = *graph.node(id);
                auto outputs = node.definition->compile(node.payload, inputs, ir);
                if (!outputs)
                {
                    return fail(std::move(outputs.error()), id);
                }
                for (std::size_t index = 0; index != outputs->size(); ++index)
                {
                    values.emplace(schema.outputs[index], (*outputs)[index]);
                }
                return true;
            }

            bool emit(NodeId id) noexcept
            {
                const auto& schema = description.nodes.at(id);
                std::vector<std::uint32_t> inputs;
                inputs.reserve(schema.inputs.size());
                for (const auto& input : schema.inputs)
                {
                    inputs.push_back(operand(input));
                    if (!ok)
                    {
                        return false;
                    }
                }
                return compile(id, inputs);
            }

            bool lower(NodeId root) noexcept
            {
                std::vector<NodeId> stack{root};
                while (ok && !stack.empty())
                {
                    const auto id = stack.back();
                    const auto state = color[id];
                    if (state == 2)
                    {
                        stack.pop_back();
                        continue;
                    }
                    if (state == 0)
                    {
                        color[id] = 1;
                        // Preserve original reverse worklist order and skip unused inputs entirely.
                        for (const auto& input : description.nodes.at(id).inputs)
                        {
                            if (input.declaration.input_use == EMaterialInputUse::UNUSED)
                            {
                                continue;
                            }
                            const auto output = source(input.id);
                            if (!output.valid())
                            {
                                continue;
                            }
                            const auto owner = description.pins.at(output)->owner;
                            const auto source_color = color[owner];
                            if (source_color == 1)
                            {
                                return fail({EMaterialCompileError::CYCLE, "cycle detected in material graph", owner});
                            }
                            if (source_color != 2)
                            {
                                stack.push_back(owner);
                            }
                        }
                    }
                    else
                    {
                        if (!emit(id))
                        {
                            return false;
                        }
                        color[id] = 2;
                        stack.pop_back();
                    }
                }
                return ok;
            }

            bool run() noexcept
            {
                NodeId surface;
                for (const auto& [id, node] : graph.nodes())
                {
                    if (node->definition->role() != EMaterialNodeRole::SURFACE)
                    {
                        continue;
                    }
                    if (surface.valid())
                    {
                        return fail(
                            {EMaterialCompileError::INVALID_GRAPH,
                             "material graph has more than one OutputSurface node",
                             id}
                        );
                    }
                    surface = id;
                }
                if (!surface.valid())
                {
                    return fail(
                        {EMaterialCompileError::MISSING_REQUIRED_OUTPUT, "material graph has no OutputSurface node"}
                    );
                }
                // Surface inputs preserve their original declaration order, independently of DFS stack order.
                color[surface] = 1;
                std::vector<std::uint32_t> inputs;
                inputs.reserve(description.nodes.at(surface).inputs.size());
                for (const auto& input : description.nodes.at(surface).inputs)
                {
                    if (input.declaration.input_use == EMaterialInputUse::UNUSED)
                    {
                        inputs.push_back(kNoValue);
                        continue;
                    }
                    const auto output = source(input.id);
                    if (output.valid())
                    {
                        const auto owner = description.pins.at(output)->owner;
                        if (color[owner] == 1)
                        {
                            return fail({EMaterialCompileError::CYCLE, "cycle detected in material graph", owner});
                        }
                        if (!lower(owner))
                        {
                            return false;
                        }
                    }
                    inputs.push_back(operand(input));
                    if (!ok)
                    {
                        return false;
                    }
                }
                return compile(surface, inputs);
            }
        };
    } // namespace

    cxx::expected<MaterialIR, MaterialCompileFailure> lowerMaterial(const MaterialGraph& graph) noexcept
    {
        MaterialIR result;
        auto description = detail::validateMaterialGraph(graph, result.shader);
        if (!description)
        {
            return cxx::unexpected(std::move(description.error()));
        }
        Lowerer lowerer{graph, *description, result.shader};
        if (!lowerer.run())
        {
            return cxx::unexpected(std::move(lowerer.error));
        }
        result.shading_model = graph.shading_model;
        result.alpha_mode = graph.render_state.alpha_mode;
        result.alpha_cutoff = graph.render_state.alpha_cutoff;
        result.double_sided = graph.render_state.double_sided;
        result.shader.fingerprint = shadergen::computeFingerprint(result.shader);
        auto fingerprint = result.shader.fingerprint;
        const auto mix = [&](std::uint64_t value) noexcept
        {
            fingerprint ^= value;
            fingerprint *= 1099511628211ULL;
        };
        mix(static_cast<std::uint32_t>(result.shading_model));
        mix(static_cast<std::uint8_t>(result.alpha_mode));
        if (result.alpha_mode == rdesc::EAlphaMode::MASK)
        {
            mix(std::bit_cast<std::uint32_t>(result.alpha_cutoff));
        }
        result.combined_fingerprint = fingerprint;
        return result;
    }
} // namespace lux::material
