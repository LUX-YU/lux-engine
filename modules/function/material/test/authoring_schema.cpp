#include "MaterialTest.hpp"
#include <lux/engine/material/BuiltinMaterialNodes.hpp>
#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>

#include <algorithm>
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
            std::fprintf(stderr, "authoring schema contract failed at line %u\n", location.line());
            std::abort();
        }
    }

    bool draft(
        const MaterialNodeType& type,
        MaterialNodePayload& payload,
        MaterialNode node,
        std::size_t pin_count
    ) noexcept
    {
        MaterialSource source{asset::AssetId{std::array<std::uint8_t, 16>{1}}, "editable draft", {}};
        const auto id = material_test::add(source.graph, std::move(node));
        // Disconnected draft nodes are still serialized and validated by the real compiler.
        static_cast<void>(material_test::add(source.graph, MaterialOutputSurface{}));
        auto encoded = encodeMaterialSource(source);
        require(encoded.has_value());
        auto decoded = decodeMaterialSource(*encoded);
        require(decoded.has_value());
        auto reencoded = encodeMaterialSource(*decoded);
        require(reencoded && *encoded == *reencoded);
        auto lowered = lowerMaterial(decoded->graph);
        require(!lowered && lowered.error().node_id == id);
        auto validation = type.validate(payload);
        require(!validation);
        require(validation.error().code == lowered.error().code);
        require(validation.error().message == lowered.error().message);
        require(validation.error().pin_index == lowered.error().pin_index);
        shadergen::ShaderIR ir;
        const auto fingerprint = shadergen::computeFingerprint(ir);
        auto compiled = type.compile(payload, {}, ir);
        require(!compiled && compiled.error().code == validation.error().code);
        require(compiled.error().message == validation.error().message);
        require(shadergen::computeFingerprint(ir) == fingerprint);
        auto pins = type.describePins(payload);
        auto copy = payload.clone();
        require(copy.has_value());
        auto cloned_pins = type.describePins(*copy);
        const bool schema_available = pins && cloned_pins;
        if (schema_available)
        {
            require(pins->size() == pin_count && cloned_pins->size() == pin_count);
        }
        std::printf(
            "%s: source roundtrip=1, compile diagnostic retained=1, editable schema=%d\n",
            type.identity().canonical_name.c_str(),
            schema_available
        );
        return schema_available;
    }
} // namespace

int main()
{
    MaterialNodeCatalog catalog;
    const auto registrations = materialBuiltinRegistrations();
    require(catalog.add(registrations).has_value());
    const auto math_type = catalog.find(registrations[4].identity.id);
    auto math_payload = math_type->create();
    require(math_payload.has_value());
    auto& math = *math_payload->get<MaterialMath>();
    std::size_t rejected{};
    for (const auto value : std::array{
             MaterialMath{EMathOp::LERP, EValueType::VEC3},
             MaterialMath{EMathOp::DOT, EValueType::FLOAT},
             MaterialMath{EMathOp::CROSS, EValueType::FLOAT},
             MaterialMath{EMathOp::CROSS, EValueType::VEC2}
         })
    {
        math = value;
        auto node = material_test::make(math);
        rejected += !draft(*math_type, *math_payload, std::move(node), 3);
    }
    const auto swizzle_type = catalog.find(registrations[5].identity.id);
    auto swizzle_payload = swizzle_type->create();
    require(swizzle_payload.has_value());
    auto& swizzle = *swizzle_payload->get<MaterialSwizzle>();
    swizzle.source_type = EValueType::FLOAT;
    swizzle.out_type = EValueType::VEC3;
    auto node = material_test::make(swizzle);
    rejected += !draft(*swizzle_type, *swizzle_payload, std::move(node), 2);
    math.op = static_cast<EMathOp>(255);
    require(!math_type->describePins(*math_payload));
    math.op = EMathOp::ADD;
    math.operand_type = static_cast<EValueType>(255);
    require(!math_type->describePins(*math_payload));
    swizzle.components[3] = 4;
    require(!swizzle_type->describePins(*swizzle_payload));
    swizzle.components[3] = 3;
    swizzle.source_type = static_cast<EValueType>(255);
    require(!swizzle_type->describePins(*swizzle_payload));
    MaterialNodePayload empty;
    require(!math_type->describePins(empty));
    require(!math_type->describePins(*swizzle_payload));
    std::printf("editable drafts rejected by schema=%zu of 5\n", rejected);
    return rejected == 0 ? 0 : 42;
}
