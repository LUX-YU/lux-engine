#include "MaterialTest.hpp"

#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>

#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>

namespace
{
    using namespace lux;
    using namespace lux::material;
    using material_test::check;

    struct Outputs final
    {
        unsigned count{2};
        bool reject_clone{};
    };

    MaterialNodeResult<std::unique_ptr<Outputs>> clone(const Outputs& value) noexcept
    {
        if (value.reject_clone)
        {
            return cxx::unexpected(
                MaterialCompileFailure{EMaterialCompileError::INVALID_RESULT, "extension clone denied"}
            );
        }
        return std::make_unique<Outputs>(value);
    }

    MaterialNodeRegistration registration()
    {
        MaterialNodeRegistration result;
        result.identity = {graph::nodeTypeId("external.multiple.v1"), "external.multiple.v1", 1};
        result.payload_type = cxx::typeToken<Outputs>();
        result.create = [](const object::CodeLease& code) noexcept
        { return MaterialNodePayload::make<Outputs, &clone>(code); };
        result.validate = [](const MaterialNodePayload&) noexcept -> MaterialNodeResult<void> { return {}; };
        result.describe_pins = [](const MaterialNodePayload& payload) noexcept -> MaterialNodeRegistration::PinResult
        {
            const auto count = payload.get<Outputs>()->count;
            if (count == 0 || count > 8)
            {
                return cxx::unexpected(
                    MaterialCompileFailure{EMaterialCompileError::INVALID_GRAPH, "invalid output count"}
                );
            }
            std::vector<MaterialPinDeclaration> pins;
            for (unsigned index = 0; index != count; ++index)
            {
                pins.push_back({graph::PinSemanticId{100 + index}, "value", graph::EPinDirection::OUTPUT});
            }
            return pins;
        };
        result.compile = [](const MaterialNodePayload& payload, std::span<const std::uint32_t>, shadergen::ShaderIR& ir
                         ) noexcept -> MaterialNodeRegistration::ShaderResult
        {
            std::vector<std::uint32_t> outputs;
            for (unsigned index = 0; index != payload.get<Outputs>()->count; ++index)
            {
                outputs.push_back(static_cast<std::uint32_t>(ir.values.size()));
                shadergen::ShaderIRValue value{shadergen::EOp::CONSTANT, shadergen::EValueType::FLOAT};
                value.constant[0] = static_cast<float>(index + 1) * 0.125F;
                ir.values.push_back(value);
            }
            return outputs;
        };
        result.encode = [](const MaterialNodePayload& payload) noexcept -> MaterialNodeResult<std::string>
        { return std::to_string(payload.get<Outputs>()->count); };
        result.decode = [](std::string_view bytes,
                           const object::CodeLease& code) noexcept -> MaterialNodeResult<MaterialNodePayload>
        {
            unsigned count{};
            const auto parsed = std::from_chars(bytes.data(), bytes.data() + bytes.size(), count);
            const bool invalid = parsed.ec != std::errc{} || parsed.ptr != bytes.data() + bytes.size();
            if (invalid)
            {
                return cxx::unexpected(
                    MaterialCompileFailure{EMaterialCompileError::INVALID_GRAPH, "invalid extension source"}
                );
            }
            return MaterialNodePayload::make<Outputs, &clone>(code, Outputs{count});
        };
        return result;
    }

    asset::AssetId assetId()
    {
        return asset::AssetId{std::array<std::uint8_t, 16>{1}};
    }

    void registeredGraph()
    {
        MaterialNodeCatalog catalog;
        const auto extension = registration();
        const auto builtins = materialBuiltinRegistrations();
        check(catalog.add(builtins).has_value());
        check(catalog.add({&extension, 1}).has_value());
        auto definition = catalog.find(extension.identity.id);
        auto payload = definition->create();
        check(payload.has_value());
        payload->get<Outputs>()->count = 4;
        MaterialSource source{assetId(), "extension graph", {}};
        const auto id = material_test::add(source.graph, MaterialNode{definition, "four outputs", std::move(*payload)});
        const auto surface = material_test::add(source.graph, MaterialOutputSurface{});
        const auto output = source.graph.pinId(id, graph::PinSemanticId{103});
        check(source.graph.connect(output, material_test::input(source.graph, surface, 3)).has_value());
        auto ir = lowerMaterial(source.graph);
        check(ir.has_value() && ir->shader.values.size() == 4);
        check(ir->shader.outputs[3].value_id == 3 && ir->shader.values[3].constant[0] == 0.5F);
        const auto encoded = encodeMaterialSource(source);
        check(encoded.has_value() && encoded->find("external.multiple.v1") != std::string::npos);
        auto absent = decodeMaterialSource(*encoded);
        check(!absent && absent.error().code == EMaterialSourceError::UNKNOWN_NODE_KIND);
        auto decoded = decodeMaterialSource(*encoded, catalog);
        check(decoded.has_value() && *encodeMaterialSource(*decoded) == *encoded);
        auto decoded_ir = lowerMaterial(decoded->graph);
        check(decoded_ir.has_value() && decoded_ir->combined_fingerprint == ir->combined_fingerprint);
        auto wrong_version = *encoded;
        const auto version = wrong_version.find("type_version = 1");
        check(version != std::string::npos);
        wrong_version.replace(version, std::string_view{"type_version = 1"}.size(), "type_version = 999");
        auto unsupported = decodeMaterialSource(wrong_version, catalog);
        check(!unsupported && unsupported.error().code == EMaterialSourceError::UNSUPPORTED_FORMAT);

        auto rejected_payload = definition->create();
        check(rejected_payload.has_value());
        rejected_payload->get<Outputs>()->reject_clone = true;
        MaterialNode denied{definition, "clone rejection", std::move(*rejected_payload)};
        const MaterialNodeEntry entry{{}, &denied};
        MaterialGraphChange change;
        change.insert = {&entry, 1};
        auto rejected = MaterialGraphEdit::prepare(source.graph, change);
        check(!rejected && std::holds_alternative<MaterialCompileFailure>(rejected.error()));
        check(std::get<MaterialCompileFailure>(rejected.error()).message == "extension clone denied");
        check(*encodeMaterialSource(source) == *encoded);
        auto copied = source.graph.clone();
        check(copied.has_value());
        check(copied->node(id)->payload.get<Outputs>() != source.graph.node(id)->payload.get<Outputs>());
    }

    void mixedIdentity()
    {
        MaterialGraph graph;
        auto fresh = material_test::make(MaterialConstant{});
        auto restored = material_test::make(MaterialConstant{});
        auto explicit_fresh = material_test::make(MaterialConstant{});
        constexpr graph::PinSemanticId Output{(std::uint64_t{1} << 63) | 1};
        const MaterialPinEntry preserved{
            {PinId{1000003}, NodeId{1001}, graph::EPinDirection::OUTPUT, graph::kUnlimitedFan, Output},
            {"old", EValueType::VEC4}
        };
        const std::array<MaterialNodeEntry, 3> entries{
            {{{}, &fresh}, {NodeId{999}, &explicit_fresh}, {NodeId{1001}, &restored, {&preserved, 1}}}
        };
        MaterialGraphChange change;
        change.insert = entries;
        NodeId abandoned;
        PinId abandoned_pin;
        {
            auto edit = MaterialGraphEdit::prepare(graph, change);
            check(edit.has_value());
            const auto assigned = edit->insertedNodes();
            abandoned = assigned[0].id;
            abandoned_pin = assigned[0].pins.front().record.id;
            check(abandoned.value > 1001 && abandoned_pin.value > preserved.record.id.value);
            check(assigned[1].pins.front().record.id.value > preserved.record.id.value);
            check(assigned[2].pins.front().record.id == preserved.record.id);
            check(graph.nodes().empty() && graph.topology().pins().empty());
        }
        const auto next = material_test::add(graph, MaterialConstant{});
        check(next.value > abandoned.value);
        check(material_test::output(graph, next).value > abandoned_pin.value);

        MaterialGraph exhausted;
        auto node = material_test::make(MaterialConstant{});
        constexpr auto Maximum = std::numeric_limits<std::uint64_t>::max();
        const MaterialPinEntry final_pin{
            {PinId{Maximum}, NodeId{Maximum}, graph::EPinDirection::OUTPUT, graph::kUnlimitedFan, Output},
            {"last", EValueType::VEC4}
        };
        check(exhausted.addNodeWithId(NodeId{Maximum}, std::move(node), {&final_pin, 1}).has_value());
        auto removed = exhausted.extractNode(NodeId{Maximum});
        check(removed.has_value());
        auto denied = exhausted.addNode(material_test::make(MaterialConstant{}));
        check(
            !denied &&
            std::get<graph::GraphTopologyFailure>(denied.error()).code == graph::EGraphTopologyError::ID_EXHAUSTED
        );
    }

    void legacy(const char* file)
    {
        std::ifstream input{file, std::ios::binary};
        check(input.good());
        const std::string bytes{std::istreambuf_iterator<char>{input}, {}};
        auto source = decodeMaterialSource(bytes);
        check(source.has_value() && source->graph.nodes().size() == 10);
        const auto encoded = encodeMaterialSource(*source);
        check(encoded.has_value() && encoded->find("version = 2") != std::string::npos);
        auto decoded = decodeMaterialSource(*encoded);
        check(decoded.has_value() && *encodeMaterialSource(*decoded) == *encoded);
        check(std::ranges::equal(source->graph.topology().nodes(), decoded->graph.topology().nodes()));
        check(std::ranges::equal(source->graph.topology().pins(), decoded->graph.topology().pins()));
        check(std::ranges::equal(source->graph.layout().all(), decoded->graph.layout().all()));
    }
} // namespace

int main(int argc, char** argv)
{
    check(argc == 2);
    registeredGraph();
    mixedIdentity();
    legacy(argv[1]);
    std::puts("registered graph: multi-output compiler, dynamic schema, canonical codec, old source, clone failure and "
              "mixed identity PASS");
}
