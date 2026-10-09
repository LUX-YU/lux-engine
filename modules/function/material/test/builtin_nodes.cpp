#include <lux/engine/material/BuiltinMaterialNodes.hpp>
#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::material;
    using ShaderType = shadergen::EValueType;

    void require(bool condition, std::source_location location = std::source_location::current()) noexcept
    {
        if (!condition)
        {
            std::fprintf(stderr, "builtin Material contract failed at line %u\n", location.line());
            std::abort();
        }
    }

    using Types = std::array<std::shared_ptr<const MaterialNodeType>, 10>;

    Types definitions()
    {
        MaterialNodeCatalog catalog;
        auto registrations = materialBuiltinRegistrations();
        require(catalog.add(registrations).has_value());
        require(!catalog.add(registrations));
        Types types;
        for (std::size_t index = 0; index != types.size(); ++index)
        {
            types[index] = catalog.find(registrations[index].identity.id);
            require(types[index] != nullptr);
            require(types[index]->identity().id == graph::nodeTypeId(types[index]->identity().canonical_name));
        }
        return types;
    }

    template <class T> void compare(const MaterialNodeType& type, T value, std::unique_ptr<Node> node)
    {
        auto payload = type.create();
        require(payload.has_value() && payload->get<T>() != nullptr);
        *payload->get<T>() = value;
        auto declared = type.describePins(*payload);
        require(declared.has_value());
        auto copy = payload->clone();
        require(copy.has_value() && copy->get<T>() != payload->get<T>());
        const auto input_count = node->inputs().size();
        MaterialGraph graph;
        graph.texture_slots.push_back({"albedo", asset::AssetId{std::array<std::uint8_t, 16>{1}}});
        auto parameter_type = EValueType::VEC4;
        if constexpr (std::is_same_v<T, MaterialParameter>)
        {
            parameter_type = value.type;
        }
        graph.param_slots.push_back({"parameter", parameter_type, {0.2F, 0.3F, 0.4F, 0.5F}});
        const auto id = graph.addNode(std::move(node));
        const auto surface = graph.addNode(std::make_unique<OutputSurfaceNode>());
        require(graph.connect(id, 0, surface, static_cast<std::uint32_t>(EMaterialAttribute::ROUGHNESS)));
        auto lowered = lowerMaterial(graph);
        require(lowered.has_value());
        auto expected = lowered->shader;
        expected.outputs.clear();
        expected.values.resize(input_count + 1);
        auto candidate = expected;
        candidate.values.resize(input_count);
        candidate.inputs.clear();
        std::array<std::uint32_t, 4> inputs{};
        for (std::size_t index = 0; index != input_count; ++index)
        {
            inputs[index] = static_cast<std::uint32_t>(index);
        }
        auto compiled = type.compile(*payload, {inputs.data(), input_count}, candidate);
        require(compiled.has_value() && compiled->size() == 1 && compiled->front() == input_count);
        require(shadergen::computeFingerprint(candidate) == shadergen::computeFingerprint(expected));
        const auto before = shadergen::computeFingerprint(candidate);
        std::array<std::uint32_t, 5> invalid_inputs;
        invalid_inputs.fill(shadergen::kNoValue);
        require(!type.compile(*payload, invalid_inputs, candidate));
        require(shadergen::computeFingerprint(candidate) == before);
    }

    void expressions(const Types& types)
    {
        for (unsigned ordinal = 0; ordinal != 4; ++ordinal)
        {
            const auto type = static_cast<EValueType>(ordinal);
            auto constant = std::make_unique<ConstantNode>();
            constant->setType(type);
            for (std::size_t index = 0; index != 4; ++index)
            {
                constant->value[index] = static_cast<float>(index) * 0.1F;
            }
            compare(*types[0], MaterialConstant{{0, 0.1F, 0.2F, 0.3F}, type}, std::move(constant));
            compare(*types[3], MaterialParameter{0, type}, std::make_unique<ParamNode>(type));
            compare(
                *types[5],
                MaterialSwizzle{EValueType::VEC4, type},
                std::make_unique<SwizzleNode>(EValueType::VEC4, type)
            );
            compare(*types[6], MaterialConstruct{type}, std::make_unique<ConstructNode>(type));
        }
        for (unsigned ordinal = 0; ordinal != static_cast<unsigned>(EMaterialInput::COUNT); ++ordinal)
        {
            const auto input = static_cast<EMaterialInput>(ordinal);
            auto node = std::make_unique<InputNode>();
            node->setInput(input);
            compare(*types[1], MaterialInput{input}, std::move(node));
        }
        compare(*types[2], MaterialSampleTexture{}, std::make_unique<SampleTextureNode>());
        compare(*types[7], MaterialDecodeNormal{}, std::make_unique<DecodeNormalNode>());
        compare(*types[8], MaterialTbnTransform{}, std::make_unique<TbnTransformNode>());
    }

    void failures(const Types& types)
    {
        shadergen::ShaderIR ir;
        auto constant = types[0]->create();
        constant->get<MaterialConstant>()->value[0] = std::numeric_limits<float>::infinity();
        require(!types[0]->validate(*constant));
        auto input = types[1]->create();
        input->get<MaterialInput>()->input = EMaterialInput::COUNT;
        require(!types[1]->describePins(*input));
        input->get<MaterialInput>()->input = EMaterialInput::UV0;
        require(types[1]->compile(*input, {}, ir).has_value());
        require(types[1]->compile(*input, {}, ir).has_value());
        require(ir.inputs.size() == 1 && ir.values[0].slot == ir.values[1].slot);
        ir.inputs[0].location = 42;
        auto before = shadergen::computeFingerprint(ir);
        require(!types[1]->compile(*input, {}, ir));
        require(before == shadergen::computeFingerprint(ir));
        auto texture = types[2]->create();
        const std::array<std::uint32_t, 1> uv{0};
        require(!types[2]->compile(*texture, uv, ir));
        require(before == shadergen::computeFingerprint(ir));
        auto parameter = types[3]->create();
        require(!types[3]->compile(*parameter, {}, ir));
        ir.params.push_back({"wrong", ShaderType::FLOAT});
        before = shadergen::computeFingerprint(ir);
        require(!types[3]->compile(*parameter, {}, ir));
        require(before == shadergen::computeFingerprint(ir));
        auto swizzle = types[5]->create();
        swizzle->get<MaterialSwizzle>()->components[2] = 4;
        auto invalid_swizzle = types[5]->validate(*swizzle);
        require(!invalid_swizzle && invalid_swizzle.error().pin_index == 2);
        auto construct = types[6]->create();
        construct->get<MaterialConstruct>()->out_type = static_cast<EValueType>(255);
        require(!types[6]->describePins(*construct));
    }

    void surface(const Types& types)
    {
        auto payload = types[9]->create();
        auto pins = types[9]->describePins(*payload);
        require(pins.has_value() && pins->size() == std::size(kMaterialAttributes));
        for (const auto& pin : *pins)
        {
            require(pin.input_use == EMaterialInputUse::CONNECTED_VALUE);
        }
        for (bool overridden : {false, true})
        {
            MaterialGraph graph;
            auto output = std::make_unique<OutputSurfaceNode>();
            if (overridden)
            {
                output->inputs()[3].constant[0] = 0.37F;
            }
            graph.addNode(std::move(output));
            auto original = lowerMaterial(graph);
            require(original.has_value());
            auto candidate = original->shader;
            candidate.outputs.clear();
            std::array<std::uint32_t, std::size(kMaterialAttributes)> inputs;
            inputs.fill(shadergen::kNoValue);
            if (overridden)
            {
                require(candidate.values.size() == 1);
                inputs[3] = 0;
            }
            else
            {
                require(candidate.values.empty());
            }
            auto result = types[9]->compile(*payload, inputs, candidate);
            require(result.has_value() && result->empty());
            require(shadergen::computeFingerprint(candidate) == shadergen::computeFingerprint(original->shader));
            const auto before = shadergen::computeFingerprint(candidate);
            require(!types[9]->compile(*payload, inputs, candidate));
            require(shadergen::computeFingerprint(candidate) == before);
        }
    }
} // namespace

int main()
{
    auto types = definitions();
    expressions(types);
    failures(types);
    surface(types);
    std::puts(
        "builtin Material registrations: graph parity, dynamic schemas, domain errors and retained definitions PASS"
    );
}
