#include "../../../../modules/function/material/test/MaterialTest.hpp"
#include <lux/engine/material/Compiler.hpp>

#include <array>
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
    constexpr std::array purposes{lux::rdesc::EMaterialPurpose::GBUFFER, lux::rdesc::EMaterialPurpose::SHADOW};
    MaterialGraph graph;
    auto empty = compileMaterial(graph, purposes);
    require(!empty && empty.error().code == EMaterialCompileError::INVALID_GRAPH);
    const auto unused = material_test::add(graph, MaterialConstant{});
    auto missing = compileMaterial(graph, purposes);
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
        auto result = compileMaterial(graph, purposes);
        if (!result)
        {
            std::fprintf(stderr, "compile failure: %s\n", result.error().message.c_str());
            return 42;
        }
        require(result->programs.programs.size() == purposes.size());
        require(result->programs.find(lux::rdesc::EMaterialPurpose::FORWARD) == nullptr);
        for (const auto& use : result->programs.programs)
        {
            for (const auto& input : use.shader.interface.entry_points.front().inputs)
            {
                require(input.location < 7);
            }
        }
        write(output, result->programs.find(lux::rdesc::EMaterialPurpose::GBUFFER)->shader.stages.front().words);
        write(output, result->programs.find(lux::rdesc::EMaterialPurpose::SHADOW)->shader.stages.front().words);
        require(result->programs.double_sided == graph.render_state.double_sided);
        require(result->programs.parameter_defaults.size() == graph.param_slots.size());
    }
    using Purpose = lux::rdesc::EMaterialPurpose;
    constexpr std::array forward{Purpose::FORWARD};
    constexpr std::array gbuffer{Purpose::GBUFFER};
    constexpr std::array shadow{Purpose::SHADOW};
    auto unsupported = compileMaterial(graph, forward);
    require(!unsupported && unsupported.error().code == EMaterialCompileError::SHADER_EMISSION_FAILURE);
    graph.shading_model = lux::rdesc::ELightingTechnique::STYLIZED;
    require(!compileMaterial(graph, forward));
    require(!compileMaterial(graph, gbuffer));
    require(compileMaterial(graph, shadow).has_value());

    graph.shading_model = lux::rdesc::ELightingTechnique::UNLIT;
    auto unlit = compileMaterial(graph, forward);
    require(unlit.has_value() && unlit->programs.programs.size() == 1);
    require(unlit->programs.find(Purpose::GBUFFER) == nullptr);
    auto invalid_purposes = compileMaterial(graph, {});
    require(!invalid_purposes && invalid_purposes.error().code == EMaterialCompileError::INVALID_PURPOSES);
    invalid_purposes = compileMaterial(graph, std::array{Purpose::FORWARD, Purpose::FORWARD});
    require(!invalid_purposes && invalid_purposes.error().code == EMaterialCompileError::INVALID_PURPOSES);
    invalid_purposes = compileMaterial(graph, std::array{static_cast<Purpose>(255)});
    require(!invalid_purposes && invalid_purposes.error().code == EMaterialCompileError::INVALID_PURPOSES);

    // Roughness is authored but does not belong to Unlit or opaque Shadow.
    // These uses must not retain its parameter table or its geometry inputs.
    graph.render_state.alpha_mode = lux::rdesc::EAlphaMode::OPAQUE_SURFACE;
    auto opaque_shadow = compileMaterial(graph, shadow);
    require(opaque_shadow.has_value());
    const auto& shadow_interface = opaque_shadow->programs.programs.front().shader.interface;
    require(shadow_interface.sets.empty() && shadow_interface.entry_points.front().inputs.empty());
    require(opaque_shadow->programs.parameter_defaults.empty() && opaque_shadow->programs.texture_count == 0);
    unlit = compileMaterial(graph, forward);
    require(unlit.has_value() && unlit->programs.programs.front().shader.interface.sets.empty());
    require(unlit->programs.parameter_defaults.empty());
    // Extension compilers may return a valid output index with malformed operands.
    // Purpose pruning must reject it before indexing the dependency marks.
    auto registrations = materialBuiltinRegistrations();
    auto registration = *std::ranges::find(
        registrations,
        MaterialConstant::TypeName,
        [](const auto& value) { return value.identity.canonical_name; }
    );
    registration.compile = [](const MaterialNodePayload& payload,
                              std::span<const std::uint32_t>,
                              lux::shadergen::ShaderIR& ir) noexcept -> MaterialNodeResult<std::vector<std::uint32_t>>
    {
        const auto index = static_cast<std::uint32_t>(ir.values.size());
        lux::shadergen::ShaderIRValue value{lux::shadergen::EOp::ADD, lux::shadergen::EValueType::VEC4};
        value.operands[0] = payload.get<MaterialConstant>()->value[0] == 0 ? index : UINT32_MAX - 1;
        value.operands[1] = value.operands[0];
        ir.values.push_back(value);
        return std::vector{index};
    };
    MaterialNodeCatalog catalog;
    require(catalog.add(std::span(&registration, 1)).has_value());
    auto definition = catalog.find(registration.identity.id);
    for (float malformed : {0.0F, 1.0F})
    {
        MaterialGraph invalid_ir;
        auto payload = definition->create();
        require(payload.has_value());
        payload->get<MaterialConstant>()->value[0] = malformed;
        auto value = material_test::add(invalid_ir, MaterialNode{definition, "invalid", std::move(*payload)});
        auto target = material_test::add(invalid_ir, MaterialOutputSurface{});
        require(material_test::connect(invalid_ir, value, 0, target, 0));
        auto rejected = compileMaterial(invalid_ir, gbuffer);
        require(!rejected && rejected.error().code == EMaterialCompileError::SHADER_EMISSION_FAILURE);
    }
    const auto invalid_id = material_test::add(graph, MaterialSampleTexture{});
    auto failure = compileMaterial(graph, purposes);
    require(!failure && failure.error().code == EMaterialCompileError::INVALID_GRAPH);
    require(failure.error().node_id == invalid_id);
    output.close();
    require(output.good());
    std::puts("PASS: actual Material compiler, four graphs/eight SPIR-V passes and retained domain errors");
}
