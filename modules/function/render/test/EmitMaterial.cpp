#include "../../material/test/MaterialTest.hpp"
#include <lux/engine/material/Compiler.hpp>

#include <fstream>

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3)
    {
        return 2;
    }
    using namespace lux::material;
    MaterialGraph graph;
    graph.shading_model = lux::rdesc::ELightingTechnique::UNLIT;
    graph.param_slots.push_back({"tint", EValueType::VEC4, {0.25F, 0.5F, 0.75F, 1.0F}});
    const auto parameter = material_test::add(graph, MaterialParameter{0, EValueType::VEC4});
    const auto surface = material_test::add(graph, MaterialOutputSurface{});
    if (argc == 3)
    {
        graph.texture_slots.push_back({"albedo", lux::asset::AssetId{std::array<std::uint8_t, 16>{1}}});
        const auto uv = material_test::add(graph, MaterialInput{EMaterialInput::UV0});
        const auto sample = material_test::add(graph, MaterialSampleTexture{});
        const auto multiply = material_test::add(graph, MaterialMath{EMathOp::MUL, EValueType::VEC4});
        material_test::check(material_test::connect(graph, uv, 0, sample, 0));
        material_test::check(material_test::connect(graph, parameter, 0, multiply, 0));
        material_test::check(material_test::connect(graph, sample, 0, multiply, 1));
        material_test::check(material_test::connect(graph, multiply, 0, surface, 0));
    }
    else
    {
        material_test::check(material_test::connect(graph, parameter, 0, surface, 0));
    }
    auto compiled = compileMaterial(graph);
    if (!compiled)
    {
        std::fprintf(stderr, "%s\n", compiled.error().message.c_str());
    }
    material_test::check(compiled.has_value());
    const auto& words = compiled->programs.find(lux::rdesc::EMaterialPurpose::FORWARD)->shader.stages.front().words;
    std::ofstream output(argv[1], std::ios::binary);
    output.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    return output ? 0 : 3;
}
