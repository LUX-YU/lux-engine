#include "MaterialTest.hpp"
#include <lux/engine/material/MaterialMath.hpp>
#include <lux/engine/material/MaterialNodeCatalog.hpp>
#include <lux/engine/material/compiler/MaterialCompiler.hpp>
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
        auto lowered = generateMaterialSlang(graph, lux::rdesc::EMaterialPurpose::GBUFFER);
        auto pins = type->describePins(*payload);
        if (math->op == EMathOp::LERP)
        {
            require(!lowered && pins && pins->size() == 3);
            std::string candidate;
            auto compiled = type->emitSlang(*payload, {}, {}, candidate);
            require(!compiled && candidate.empty());
            require(lowered.error().code == compiled.error().code);
            require(lowered.error().message == compiled.error().message);
            continue;
        }
        require(lowered.has_value() && pins.has_value());
        require(pins->size() == 3);
        require((*pins)[0].input_use == EMaterialInputUse::VALUE);
        const bool unary = (*pins)[1].input_use == EMaterialInputUse::UNUSED;
        std::array<std::string_view, 2> inputs{"float3(1)", unary ? "" : "float3(2)"};
        std::string_view output_names[]{"result"};
        std::string candidate;
        require(type->emitSlang(*payload, inputs, output_names, candidate).has_value());
        const auto before = candidate;
        inputs[1] = unary ? inputs[0] : "";
        require(!type->emitSlang(*payload, inputs, output_names, candidate));
        require(candidate == before);
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
