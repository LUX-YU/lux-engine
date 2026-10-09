#include "MaterialTest.hpp"
#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/MaterialMath.hpp>
#include <lux/engine/material/MaterialNodeCatalog.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::material;

    void require(bool condition, std::source_location location = std::source_location::current()) noexcept
    {
        if (!condition)
        {
            std::fprintf(stderr, "math registration contract failed at line %u\n", location.line());
            std::abort();
        }
    }
} // namespace

int main()
{
    std::shared_ptr<const MaterialNodeType> type;
    {
        MaterialNodeCatalog catalog;
        auto registration = materialMathRegistration();
        require(registration.identity.canonical_name == "lux.material.math.v1");
        require(catalog.add({&registration, 1}).has_value());
        type = catalog.find(registration.identity.id);
    }
    auto payload = type->create();
    require(payload.has_value());
    auto* math = payload->get<MaterialMath>();
    require(math != nullptr);
    math->operand_type = EValueType::VEC3;
    std::size_t checked{};
    for (unsigned ordinal = 0; ordinal <= static_cast<unsigned>(EMathOp::LENGTH); ++ordinal)
    {
        math->op = static_cast<EMathOp>(ordinal);
        auto node = material_test::make(*math);
        MaterialGraph graph;
        const auto id = material_test::add(graph, std::move(node));
        const auto output = material_test::add(graph, MaterialOutputSurface{});
        require(material_test::connect(graph, id, 0, output, 0));
        auto lowered = lowerMaterial(graph);
        auto pins = type->describePins(*payload);
        if (math->op == EMathOp::LERP)
        {
            require(!lowered && pins && pins->size() == 3);
            shadergen::ShaderIR candidate;
            auto compiled = type->compile(*payload, {}, candidate);
            require(!compiled && candidate.values.empty());
            require(lowered.error().code == compiled.error().code);
            require(lowered.error().message == compiled.error().message);
            continue;
        }
        require(lowered.has_value() && pins.has_value());
        require(pins->size() == 3);
        require((*pins)[0].input_use == EMaterialInputUse::VALUE);
        const bool unary = (*pins)[1].input_use == EMaterialInputUse::UNUSED;
        const auto position = unary ? 1U : 2U;
        const auto& original = lowered->shader.values[position];
        require(original.type == static_cast<shadergen::EValueType>((*pins)[2].type));
        std::array<std::uint32_t, 2> inputs{original.operands[0], shadergen::kNoValue};
        if (!unary)
        {
            inputs[1] = original.operands[1];
        }
        auto candidate = lowered->shader;
        candidate.outputs.clear();
        candidate.values.resize(position);
        auto compiled = type->compile(*payload, inputs, candidate);
        require(compiled.has_value() && compiled->size() == 1 && compiled->front() == position);
        auto expected = lowered->shader;
        expected.outputs.clear();
        expected.values.resize(position + 1);
        require(shadergen::computeFingerprint(candidate) == shadergen::computeFingerprint(expected));
        const auto before = shadergen::computeFingerprint(candidate);
        inputs[1] = unary ? inputs[0] : shadergen::kNoValue;
        require(!type->compile(*payload, inputs, candidate));
        require(shadergen::computeFingerprint(candidate) == before);
        ++checked;
    }
    require(checked == 22);
    math->op = EMathOp::SATURATE;
    auto copy = payload->clone();
    require(copy.has_value() && copy->get<MaterialMath>() != math);
    math->op = EMathOp::DOT;
    require(copy->get<MaterialMath>()->op == EMathOp::SATURATE);
    math->operand_type = EValueType::FLOAT;
    require(!type->validate(*payload));
    math->op = static_cast<EMathOp>(255);
    require(!type->validate(*payload));
    std::puts("registered Math: 22 actual graph emissions, unused input, scalar output, clone and failures PASS");
}
