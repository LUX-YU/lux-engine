#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>

namespace
{
    lux::asset::AssetId id(std::uint8_t value)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = value;
        return lux::asset::AssetId{bytes};
    }

    lux::material::MaterialGraph validGraph()
    {
        using namespace lux::material;
        MaterialGraph graph;
        auto constant = std::make_unique<ConstantNode>();
        constant->setType(EValueType::VEC3);
        constant->value[0] = 0.25F;
        constant->value[1] = 0.5F;
        constant->value[2] = 0.75F;
        const auto value = graph.addNode(std::move(constant));
        const auto output = graph.addNode(std::make_unique<OutputSurfaceNode>());
        assert(graph.connect(value, 0U, output, static_cast<std::uint32_t>(EMaterialAttribute::BASE_COLOR)));
        return graph;
    }

    lux::material::OutputSurfaceNode& outputNode(lux::material::MaterialGraph& graph)
    {
        for (const auto& [unused, node] : graph.nodes())
            if (auto* result = node->as<lux::material::OutputSurfaceNode>()) return *result;
        std::abort();
    }
}

int main()
{
    using namespace lux::material;

    auto graph = validGraph();
    const auto compiled = compileMaterial(graph);
    assert(compiled);
    assert(compiled->gbuffer_spirv.size() >= 5U);
    assert(compiled->forward_spirv.size() >= 5U);

    MaterialGraph parameter_graph;
    parameter_graph.param_slots.push_back(ParamSlotDecl{"base-color", EValueType::VEC3,
                                                        {0.2F, 0.4F, 0.6F, 0.0F}});
    auto parameter = std::make_unique<ParamNode>(EValueType::VEC3);
    parameter->param_slot = 0U;
    const auto parameter_id = parameter_graph.addNode(std::move(parameter));
    const auto parameter_output = parameter_graph.addNode(std::make_unique<OutputSurfaceNode>());
    assert(parameter_graph.connect(parameter_id, 0U, parameter_output,
                                   static_cast<std::uint32_t>(EMaterialAttribute::BASE_COLOR)));
    assert(compileMaterial(parameter_graph));

    MaterialGraph texture_graph;
    texture_graph.texture_slots.push_back(TextureSlotDecl{"base-color", id(1U)});
    const auto uv = texture_graph.addNode(std::make_unique<InputNode>());
    const auto sample = texture_graph.addNode(std::make_unique<SampleTextureNode>());
    const auto swizzle = texture_graph.addNode(std::make_unique<SwizzleNode>());
    const auto texture_output = texture_graph.addNode(std::make_unique<OutputSurfaceNode>());
    assert(texture_graph.connect(uv, 0U, sample, 0U));
    assert(texture_graph.connect(sample, 0U, swizzle, 0U));
    assert(texture_graph.connect(swizzle, 0U, texture_output,
                                 static_cast<std::uint32_t>(EMaterialAttribute::BASE_COLOR)));
    texture_graph.shading_model = lux::rdesc::ELightingTechnique::Stylized;
    texture_graph.render_state.alpha_mode = lux::rdesc::EAlphaMode::Mask;
    texture_graph.render_state.alpha_cutoff = 0.4F;
    assert(compileMaterial(texture_graph));

    MaterialGraph missing_output;
    missing_output.addNode(std::make_unique<ConstantNode>());
    const auto missing = compileMaterial(missing_output);
    assert(!missing);
    assert(missing.error().code == EMaterialCompileError::MISSING_REQUIRED_OUTPUT);

    MaterialGraph cycle;
    const auto left = cycle.addNode(std::make_unique<MathNode>());
    const auto right = cycle.addNode(std::make_unique<MathNode>());
    const auto output = cycle.addNode(std::make_unique<OutputSurfaceNode>());
    assert(cycle.connect(left, 0U, right, 0U));
    assert(cycle.connect(right, 0U, left, 0U));
    assert(cycle.connect(left, 0U, output, static_cast<std::uint32_t>(EMaterialAttribute::METALLIC)));
    const auto cyclic = compileMaterial(cycle);
    assert(!cyclic);
    assert(cyclic.error().code == EMaterialCompileError::CYCLE);
    assert(cyclic.error().node_id != invalid_node);

    auto invalid_reference = validGraph();
    auto& invalid_reference_pin =
        outputNode(invalid_reference).inputs()[static_cast<std::uint32_t>(EMaterialAttribute::BASE_COLOR)];
    invalid_reference_pin.source = PinLink{999U, 0U};
    const auto dangling = compileMaterial(invalid_reference);
    assert(!dangling && dangling.error().code == EMaterialCompileError::INVALID_GRAPH);
    assert(dangling.error().node_id == 999U);

    auto invalid_pin = validGraph();
    auto& invalid_output_pin =
        outputNode(invalid_pin).inputs()[static_cast<std::uint32_t>(EMaterialAttribute::BASE_COLOR)];
    invalid_output_pin.source.pin = 7U;
    const auto bad_pin = compileMaterial(invalid_pin);
    assert(!bad_pin && bad_pin.error().code == EMaterialCompileError::INVALID_GRAPH);
    assert(bad_pin.error().pin_index == 7U);

    MaterialGraph mismatch;
    auto vec2 = std::make_unique<ConstantNode>();
    vec2->setType(EValueType::VEC2);
    const auto vec2_id = mismatch.addNode(std::move(vec2));
    const auto mismatch_output = mismatch.addNode(std::make_unique<OutputSurfaceNode>());
    assert(mismatch.connect(vec2_id, 0U, mismatch_output,
                            static_cast<std::uint32_t>(EMaterialAttribute::BASE_COLOR)));
    const auto type_mismatch = compileMaterial(mismatch);
    assert(!type_mismatch && type_mismatch.error().code == EMaterialCompileError::TYPE_MISMATCH);

    MaterialGraph invalid_texture_slot;
    const auto uv_node = invalid_texture_slot.addNode(std::make_unique<InputNode>());
    auto invalid_sample = std::make_unique<SampleTextureNode>();
    invalid_sample->texture_slot = 3U;
    const auto invalid_sample_id = invalid_texture_slot.addNode(std::move(invalid_sample));
    const auto invalid_texture_output = invalid_texture_slot.addNode(std::make_unique<OutputSurfaceNode>());
    assert(invalid_texture_slot.connect(uv_node, 0U, invalid_sample_id, 0U));
    assert(invalid_texture_slot.connect(invalid_sample_id, 0U, invalid_texture_output,
                                        static_cast<std::uint32_t>(EMaterialAttribute::BASE_COLOR)));
    const auto bad_texture_slot = compileMaterial(invalid_texture_slot);
    assert(!bad_texture_slot && bad_texture_slot.error().code == EMaterialCompileError::INVALID_GRAPH);

    MaterialGraph invalid_parameter_slot;
    auto invalid_parameter = std::make_unique<ParamNode>(EValueType::FLOAT);
    invalid_parameter->param_slot = 2U;
    const auto invalid_parameter_id = invalid_parameter_slot.addNode(std::move(invalid_parameter));
    const auto invalid_parameter_output = invalid_parameter_slot.addNode(std::make_unique<OutputSurfaceNode>());
    assert(invalid_parameter_slot.connect(invalid_parameter_id, 0U, invalid_parameter_output,
                                          static_cast<std::uint32_t>(EMaterialAttribute::METALLIC)));
    const auto bad_parameter_slot = compileMaterial(invalid_parameter_slot);
    assert(!bad_parameter_slot && bad_parameter_slot.error().code == EMaterialCompileError::INVALID_GRAPH);

    auto null_texture = validGraph();
    null_texture.texture_slots.push_back(TextureSlotDecl{"null", {}});
    const auto null_texture_result = compileMaterial(null_texture);
    assert(!null_texture_result && null_texture_result.error().code == EMaterialCompileError::INVALID_GRAPH);

    auto invalid_default = validGraph();
    invalid_default.param_slots.push_back(ParamSlotDecl{
        "invalid",
        EValueType::FLOAT,
        {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 0.0F}
    });
    const auto non_finite = compileMaterial(invalid_default);
    assert(!non_finite);
    assert(non_finite.error().code == EMaterialCompileError::INVALID_GRAPH);
    return 0;
}
