#include "../../../../modules/function/material/test/MaterialTest.hpp"
#include <lux/engine/material/Compiler.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <source_location>

namespace
{
    using namespace lux::material;

    void require(bool condition, std::source_location location = std::source_location::current()) noexcept
    {
        if (!condition)
        {
            std::fprintf(stderr, "require failed at line %u\n", location.line());
            std::abort();
        }
    }

    void write(std::ofstream& output, const std::vector<std::uint32_t>& words)
    {
        require(words.size() > 5 && words.front() == 0x07230203U);
        const auto size = static_cast<std::uint64_t>(words.size());
        output.write(reinterpret_cast<const char*>(&size), sizeof(size));
        output.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    }
} // namespace

int main(int argc, char** argv)
{
    require(argc == 2);
    std::ofstream output{std::filesystem::path{argv[1]}, std::ios::binary};
    require(output.good());
    MaterialGraph graph;
    auto empty = compileMaterial(graph);
    require(!empty && empty.error().code == EMaterialCompileError::INVALID_GRAPH);
    const auto unused = material_test::add(graph, MaterialConstant{});
    auto missing = compileMaterial(graph);
    require(!missing && missing.error().code == EMaterialCompileError::MISSING_REQUIRED_OUTPUT);
    require(graph.removeNode(unused).has_value());
    const auto surface = material_test::add(graph, MaterialOutputSurface{});
    for (int mode{}; mode != 4; ++mode)
    {
        if (mode == 1)
        {
            const auto value = material_test::add(graph, MaterialConstant{{0.2F, 0.5F, 0.7F, 0}, EValueType::VEC3});
            require(material_test::connect(
                graph,
                value,
                0,
                surface,
                static_cast<std::uint32_t>(EMaterialAttribute::BASE_COLOR)
            ));
        }
        if (mode == 2)
        {
            graph.param_slots.push_back({"roughness", EValueType::FLOAT, {0.35F, 0, 0, 0}});
            const auto value = material_test::add(graph, MaterialParameter{0, EValueType::FLOAT});
            require(material_test::connect(
                graph,
                value,
                0,
                surface,
                static_cast<std::uint32_t>(EMaterialAttribute::ROUGHNESS)
            ));
        }
        if (mode == 3)
        {
            graph.render_state.alpha_mode = lux::rdesc::EAlphaMode::MASK;
            graph.render_state.alpha_cutoff = 0.4F;
            graph.render_state.double_sided = true;
        }
        auto result = compileMaterial(graph);
        if (!result)
        {
            std::fprintf(stderr, "compile failure: %s\n", result.error().message.c_str());
            return 42;
        }
        write(output, result->gbuffer_spirv);
        write(output, result->forward_spirv);
        require(result->double_sided == graph.render_state.double_sided);
        require(result->parameter_count == graph.param_slots.size());
    }
    const auto invalid_id = material_test::add(graph, MaterialSampleTexture{});
    auto failure = compileMaterial(graph);
    require(!failure && failure.error().code == EMaterialCompileError::INVALID_GRAPH);
    require(failure.error().node_id == invalid_id);
    output.close();
    require(output.good());
    std::puts("PASS: actual Material compiler, four graphs/eight SPIR-V passes and retained domain errors");
}
