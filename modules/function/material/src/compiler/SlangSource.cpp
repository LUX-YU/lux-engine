#include <algorithm>
#include <lux/engine/material/BuiltinMaterialNodes.hpp>
#include <lux/engine/material/compiler/MaterialCompiler.hpp>
#include <lux/engine/material/compiler/ShaderSources.hpp>
#include <lux/engine/material/detail/MaterialValidation.hpp>
#include <lux/engine/material/detail/SlangText.hpp>
#include <map>

namespace lux::material
{
    namespace
    {
        // Only compilation-local graph indexes and stable text names; no expression representation.
        struct SourceWriter final
        {
            const MaterialGraph& graph;
            const detail::MaterialCompileGraph& description;
            std::unordered_map<NodeId, unsigned> colors;
            std::unordered_map<PinId, std::string> names;
            std::string body;
            MaterialCompileFailure error;
            bool ok = true;
            bool uses_material = false, uses_textures = false;

            struct Input
            {
                std::string type;
                bool flat = false;
            };

            std::map<std::int32_t, Input> inputs;

            bool fail(EMaterialCompileError code, std::string message, NodeId node = {}, unsigned pin = ~0u)
            {
                if (ok)
                {
                    error = {code, std::move(message), node, pin};
                }
                ok = false;
                return false;
            }

            bool input(std::int32_t location, std::string type, bool flat = false)
            {
                if (location < 0)
                {
                    return fail(EMaterialCompileError::SHADER_EMISSION_FAILURE, "unsupported material input");
                }
                auto [position, added] = inputs.emplace(location, Input{type, flat});
                const bool has_conflict = !added && (position->second.type != type || position->second.flat != flat);
                return !has_conflict || fail(EMaterialCompileError::SHADER_EMISSION_FAILURE, "input location conflict");
            }

            PinId source(PinId pin) const
            {
                auto found = description.incoming.find(pin);
                return found == description.incoming.end() ? PinId{} : found->second;
            }

            std::string operand(const detail::MaterialCompilePin& input)
            {
                if (input.declaration.input_use == EMaterialInputUse::UNUSED)
                {
                    return {};
                }
                const auto& pin = *graph.pin(input.id);
                auto output = source(input.id);
                if (!output.valid())
                {
                    const bool is_default = pin.constant == input.declaration.default_value;
                    if (input.declaration.input_use == EMaterialInputUse::CONNECTED_VALUE && is_default)
                    {
                        return {};
                    }
                    return detail::slangLiteral(pin.type, pin.constant.data());
                }
                auto found = names.find(output);
                if (found == names.end())
                {
                    fail(EMaterialCompileError::SHADER_EMISSION_FAILURE, "input was not emitted");
                    return {};
                }
                const auto produced = graph.pin(output)->type;
                if (produced == pin.type)
                {
                    return found->second;
                }
                if (produced > pin.type)
                {
                    return found->second + "." + std::string("xyzw", unsigned(pin.type) + 1);
                }
                if (produced == EValueType::FLOAT)
                {
                    return detail::slangType(pin.type) + "(" + found->second + ")";
                }
                fail(
                    EMaterialCompileError::TYPE_MISMATCH,
                    "vector widening requires Construct",
                    description.pins.at(output)->owner
                );
                return {};
            }

            bool emit(NodeId id)
            {
                const auto& node = *graph.node(id);
                const auto& schema = description.nodes.at(id);
                std::vector<std::string> in, out;
                for (const auto& pin : schema.inputs)
                {
                    in.push_back(operand(pin));
                }
                if (!ok)
                {
                    return false;
                }
                for (const auto pin : schema.outputs)
                {
                    auto name = "lux_value_" + std::to_string(pin.value);
                    body += detail::slangType(graph.pin(pin)->type) + " " + name + ";\n";
                    names.emplace(pin, name);
                    out.push_back(std::move(name));
                }
                std::vector<std::string_view> in_views(in.begin(), in.end()), out_views(out.begin(), out.end());
                body += "#line 1 \"node_" + std::to_string(id.value) + "\"\n";
                auto emitted = node.definition->emitSlang(node.payload, in_views, out_views, body);
                if (!emitted)
                {
                    error = std::move(emitted.error());
                    error.node_id = id;
                    ok = false;
                    return false;
                }
                if (const auto* value = node.payload.get<MaterialInput>())
                {
                    const auto* info = materialInputDescription(value->input);
                    if (!input(detail::kMaterialInputLocations[unsigned(value->input)], detail::slangType(info->type)))
                    {
                        error.node_id = id;
                        return false;
                    }
                }
                if (node.payload.get<MaterialTbnTransform>())
                {
                    if (!input(1, "float3") || !input(5, "float4") || !input(6, "float3"))
                    {
                        return false;
                    }
                }
                uses_textures |= node.payload.get<MaterialSampleTexture>() != nullptr;
                uses_material |= uses_textures || node.payload.get<MaterialParameter>() != nullptr;
                return true;
            }

            bool visit(NodeId root)
            {
                std::vector<NodeId> stack{root};
                while (ok && !stack.empty())
                {
                    auto id = stack.back();
                    const auto state = colors[id];
                    if (state == 2)
                    {
                        stack.pop_back();
                        continue;
                    }
                    if (state == 0)
                    {
                        colors[id] = 1;
                        for (const auto& pin : description.nodes.at(id).inputs)
                        {
                            if (pin.declaration.input_use == EMaterialInputUse::UNUSED)
                            {
                                continue;
                            }
                            const auto output = source(pin.id);
                            if (!output.valid())
                            {
                                continue;
                            }
                            const auto owner = description.pins.at(output)->owner;
                            if (colors[owner] == 1)
                            {
                                return fail(EMaterialCompileError::CYCLE, "material graph cycle", owner);
                            }
                            if (colors[owner] != 2)
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
                        colors[id] = 2;
                        stack.pop_back();
                    }
                }
                return ok;
            }
        };
    } // namespace

    MaterialNodeResult<std::string>
    generateMaterialSlang(const MaterialGraph& graph, rdesc::EMaterialPurpose purpose) noexcept
    {
        auto description = detail::validateMaterialGraph(graph);
        if (!description)
        {
            return cxx::unexpected(std::move(description.error()));
        }
        SourceWriter writer{graph, *description};
        const auto fail = [](std::string message) -> MaterialNodeResult<std::string>
        {
            return cxx::unexpected(MaterialCompileFailure{EMaterialCompileError::INVALID_PURPOSES, std::move(message)});
        };
        const bool is_shadow = purpose == rdesc::EMaterialPurpose::SHADOW;
        const bool is_gbuffer = purpose == rdesc::EMaterialPurpose::GBUFFER;
        const bool is_unlit = graph.shading_model == rdesc::ELightingTechnique::UNLIT;
        const bool is_pbr = graph.shading_model == rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
        const bool is_unsupported_model =
            (!is_shadow && !is_gbuffer && !is_unlit) || (is_gbuffer && !is_unlit && !is_pbr);
        const bool is_invalid_purpose = purpose > rdesc::EMaterialPurpose::SHADOW;
        if (is_invalid_purpose || is_unsupported_model || graph.render_state.alpha_mode == rdesc::EAlphaMode::BLEND)
        {
            return fail("material model/alpha/purpose is not supported");
        }
        NodeId surface;
        for (const auto& [id, node] : graph.nodes())
        {
            if (node->definition->role() != EMaterialNodeRole::SURFACE)
            {
                continue;
            }
            if (surface.valid())
            {
                return fail("multiple material surfaces");
            }
            surface = id;
        }
        if (!surface.valid())
        {
            return cxx::unexpected(
                MaterialCompileFailure{EMaterialCompileError::MISSING_REQUIRED_OUTPUT, "missing material surface"}
            );
        }
        auto& schema = description->nodes.at(surface);
        if (schema.inputs.size() != std::size(kMaterialAttributes) || !schema.outputs.empty())
        {
            return fail("surface does not implement the material attributes");
        }
        std::array<bool, std::size(kMaterialAttributes)> selected{};
        selected[0] = selected[5] = !is_shadow;
        selected[1] = (!is_shadow && !is_gbuffer) || graph.render_state.alpha_mode == rdesc::EAlphaMode::MASK;
        selected[2] = selected[3] = selected[4] = selected[6] = is_gbuffer && !is_unlit;
        std::vector<std::string> expressions(schema.inputs.size());
        writer.colors[surface] = 1;
        for (std::size_t index = 0; index < schema.inputs.size(); ++index)
        {
            const auto& pin = schema.inputs[index];
            const auto& attribute = kMaterialAttributes[index];
            const bool is_attribute_mismatch = pin.declaration.name != attribute.name ||
                                               pin.declaration.type != attribute.type ||
                                               pin.declaration.input_use != EMaterialInputUse::CONNECTED_VALUE;
            if (is_attribute_mismatch)
            {
                return fail("surface attribute contract mismatch");
            }
            if (!selected[index])
            {
                continue;
            }
            writer.body += detail::slangType(attribute.type) + " lux_" + attribute.name + " = " +
                           detail::slangLiteral(attribute.type, attribute.dflt) + ";\n";
            auto output = writer.source(pin.id);
            if (output.valid())
            {
                auto owner = description->pins.at(output)->owner;
                if (writer.colors[owner] == 1)
                {
                    return cxx::unexpected(MaterialCompileFailure{EMaterialCompileError::CYCLE, "surface cycle", owner}
                    );
                }
                if (!writer.visit(owner))
                {
                    return cxx::unexpected(std::move(writer.error));
                }
            }
            expressions[index] = writer.operand(pin);
            if (!writer.ok)
            {
                return cxx::unexpected(std::move(writer.error));
            }
        }
        std::vector<std::string_view> views(expressions.begin(), expressions.end());
        const auto& surface_node = *graph.node(surface);
        auto emitted = surface_node.definition->emitSlang(surface_node.payload, views, {}, writer.body);
        if (!emitted)
        {
            emitted.error().node_id = surface;
            return cxx::unexpected(std::move(emitted.error()));
        }
        writer.body += "#line 1 \"lux_material_shell\"\n";
        const bool uses_material = writer.uses_material, uses_textures = writer.uses_textures;
        if (uses_material && !writer.input(4, "uint2", true))
        {
            return cxx::unexpected(std::move(writer.error));
        }
        std::string normal = "float3(0,0,1)";
        if (is_gbuffer && !is_unlit)
        {
            if (expressions[4].empty())
            {
                if (!writer.input(1, "float3"))
                {
                    return cxx::unexpected(std::move(writer.error));
                }
                normal = "normalize(input.v1)";
            }
            else
            {
                normal = "normalize(lux_normal_world)";
            }
        }
        auto& body = writer.body;
        auto& inputs = writer.inputs;
        const auto output = [](std::string_view name) { return "lux_" + std::string(name); };
        std::string source = "// Lux material graph direct Slang; target layout is reflected independently.\n";
        source += detail::kMaterialTypesSource;
        if (is_gbuffer)
        {
            source += detail::kGBufferSource;
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
                      std::to_string(std::max<std::size_t>(1, graph.param_slots.size())) + "]; TextureRefGPU tex[" +
                      std::to_string(std::max<std::size_t>(1, graph.texture_slots.size())) + "]; };\n";
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
        if (graph.render_state.alpha_mode == rdesc::EAlphaMode::MASK)
        {
            source += "if (" + output("opacity") + " < " + detail::slangNumber(graph.render_state.alpha_cutoff) +
                      ") discard;\n";
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
} // namespace lux::material
