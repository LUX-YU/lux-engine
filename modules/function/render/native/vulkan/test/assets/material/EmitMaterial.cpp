#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/compiler/Backend.hpp>
#include "../../../../../../material/test/MaterialTest.hpp"

#include <fstream>

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    using namespace lux::material;
    MaterialGraph graph;
    graph.shading_model = lux::rdesc::ELightingTechnique::UNLIT;
    graph.param_slots.push_back({"tint", EValueType::VEC4, {0.25F, 0.5F, 0.75F, 1.0F}});
    const auto parameter = material_test::add(graph, MaterialParameter{0, EValueType::VEC4});
    const auto surface = material_test::add(graph, MaterialOutputSurface{});
    material_test::check(material_test::connect(graph, parameter, 0, surface, 0));
    auto lowered = lowerMaterial(graph);
    material_test::check(lowered.has_value());
    const lux::shadergen::glsl::EmitParams options{
        lux::shadergen::glsl::EMaterialPass::FORWARD, lowered->shading_model,
        lowered->alpha_mode, lowered->alpha_cutoff
    };
    auto source = lux::shadergen::glsl::emitGlsl(lowered->shader, options);
    material_test::check(source.has_value());
    material_test::check(source->find("uMats") != std::string::npos);
    std::ofstream output(argv[1], std::ios::binary);
    const auto version_end = source->find('\n');
    output << source->substr(0, version_end + 1)
        << "#extension GL_GOOGLE_include_directive : require\n" << source->substr(version_end + 1);
    return output ? 0 : 3;
}

