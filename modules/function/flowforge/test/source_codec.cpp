#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool value, std::source_location where = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "Flow source codec assertion at %u\n", where.line());
            std::abort();
        }
    }

    template <class T>
    concept HasOperation = requires(T& value) { value.operation; };
    static_assert(!HasOperation<FlowSourceNode>);

    struct Payload final
    {
        unsigned outputs{2};
        bool reject{};
    };

    FlowForgeResult<std::unique_ptr<Payload>> clone(const Payload& value) noexcept
    {
        return std::make_unique<Payload>(value);
    }

    FlowNodeRegistration registration()
    {
        FlowNodeRegistration result;
        result.identity = {graph::nodeTypeId("test.variable_outputs"), "test.variable_outputs", 3};
        result.payload_type = cxx::typeToken<Payload>();
        result.create = [](const object::CodeLease& code) noexcept
        { return FlowNodePayload::make<Payload, clone>(code); };
        result.describe_pins = [](const FlowNodePayload& payload) noexcept -> FlowNodeRegistration::PinResult
        {
            const auto count = payload.get<Payload>()->outputs;
            if (count == 0 || count > 4)
            {
                return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "invalid output count"});
            }
            std::vector<FlowPinDeclaration> pins;
            for (unsigned i{}; i != count; ++i)
            {
                pins.push_back(FlowPinDeclaration{
                    graph::PinSemanticId{100U + i},
                    "out" + std::to_string(i),
                    graph::EPinDirection::OUTPUT,
                    &meta::ref_type_of_v<int>
                });
            }
            return pins;
        };
        result.validate = [](const FlowNodePayload& payload) noexcept -> FlowForgeResult<void>
        {
            if (payload.get<Payload>()->reject)
            {
                return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "editable draft"});
            }
            return {};
        };
        result.compile = [](const FlowNodePayload&,
                            std::span<const FlowValue>,
                            FlowValueCompiler&) noexcept -> FlowNodeRegistration::ValueResult
        { return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "not a compile fixture"}); };
        result.encode = [](const FlowNodePayload& value) noexcept -> FlowForgeResult<std::string>
        {
            const auto& payload = *value.get<Payload>();
            return std::string{static_cast<char>(payload.outputs), static_cast<char>(payload.reject), '\0', '\xff'};
        };
        result.decode = [](std::string_view bytes,
                           const object::CodeLease& code) noexcept -> FlowForgeResult<FlowNodePayload>
        {
            const bool has_size = bytes.size() == 4;
            const bool has_schema = has_size && bytes[0] > 0 && bytes[0] <= 4 && (bytes[1] == 0 || bytes[1] == 1) &&
                                    bytes[2] == 0 && static_cast<unsigned char>(bytes[3]) == 255;
            if (!has_schema)
            {
                return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "invalid source payload"});
            }
            return FlowNodePayload::make<Payload, clone>(code, static_cast<unsigned>(bytes[0]), bytes[1] != 0);
        };
        return result;
    }

    asset::AssetId assetId()
    {
        return asset::AssetId{std::array<std::uint8_t, 16>{1}};
    }

    void registeredRoundtrip()
    {
        FlowNodeCatalog catalog;
        auto entry = registration();
        require(catalog.add(std::span{&entry, 1}).has_value());
        FlowGraph graph;
        auto definition = catalog.find(entry.identity.id);
        auto payload = definition->create();
        require(payload.has_value());
        payload->get<Payload>()->outputs = 4;
        payload->get<Payload>()->reject = true;
        auto node = createFlowValueNode(definition, std::move(*payload));
        require(node.has_value());
        const auto id = graph.addNode(std::move(*node));
        require(id.valid());
        require(graph.layout().set(id, {12.0F, -3.0F, true}).has_value());
        auto source = captureFlowSource(assetId(), "extension", graph);
        require(source.has_value());
        require(source->nodes.front().type == entry.identity.canonical_name && source->nodes.front().version == 3);
        auto encoded = encodeFlowSource(*source);
        require(encoded.has_value() && encoded->find("operation =") == std::string::npos);
        require(encoded->find("version = 2") != std::string::npos);
        auto decoded = decodeFlowSource(*encoded);
        require(decoded.has_value() && *decoded == *source);
        const auto unknown = materializeFlowSource(*decoded);
        require(!unknown && unknown.error().code == EFlowSourceError::UNKNOWN_NODE_KIND);
        FlowSourceEnvironment environment;
        environment.nodes = &catalog;
        auto restored = materializeFlowSource(*decoded, environment);
        require(restored.has_value());
        const auto* actual = restored->findNodeById(id);
        require(actual && actual->outPins().size() == 4);
        require(actual->registeredPayload()->get<Payload>()->reject);
        require(!definition->validate(*actual->registeredPayload()));
        auto again = captureFlowSource(source->id, source->name, *restored);
        require(again.has_value() && *again == *source);
        require(*encodeFlowSource(*again) == *encoded);

        auto changed = *source;
        ++changed.nodes.front().version;
        auto wrong_version = materializeFlowSource(changed, environment);
        require(!wrong_version && wrong_version.error().code == EFlowSourceError::SCHEMA_MISMATCH);
        changed = *source;
        changed.nodes.front().outputs.front().semantic.value = 900;
        auto wrong_pin = materializeFlowSource(changed, environment);
        require(!wrong_pin && wrong_pin.error().code == EFlowSourceError::SCHEMA_MISMATCH);
        changed.nodes.front().outputs.front().semantic = changed.nodes.front().outputs.back().semantic;
        require(!encodeFlowSource(changed));
        changed = *source;
        std::get<FlowSourcePayload>(changed.nodes.front().parameters).bytes = "bad";
        auto bad_codec = materializeFlowSource(changed, environment);
        require(!bad_codec && bad_codec.error().code == EFlowSourceError::NODE_CODEC_FAILURE);
        require(bad_codec.error().cause && bad_codec.error().cause->message == "invalid source payload");
        auto bad_hex = *encoded;
        const auto data = bad_hex.find("040100ff");
        require(data != std::string::npos);
        bad_hex[data] = 'z';
        require(!decodeFlowSource(bad_hex));
        auto mixed_format = *encoded;
        const auto type_field = mixed_format.find("type_version =");
        require(type_field != std::string::npos);
        mixed_format.insert(type_field, "operation = 19\n");
        auto mixed = decodeFlowSource(mixed_format);
        require(!mixed && mixed.error().code == EFlowSourceError::UNKNOWN_FIELD);
        changed = *source;
        std::get<FlowSourcePayload>(changed.nodes.front().parameters).bytes.front() = 2;
        auto wrong_count = materializeFlowSource(changed, environment);
        require(!wrong_count && wrong_count.error().code == EFlowSourceError::SCHEMA_MISMATCH);
        FlowSourceLimits limits;
        limits.max_bytes = 2;
        require(!encodeFlowSource(*source, limits));
        auto reserved = registration();
        reserved.identity = {graph::nodeTypeId("lux.flow.add"), "lux.flow.add", 1};
        require(!catalog.add(std::span{&reserved, 1}));
        auto collision = registration();
        collision.identity.id = reserved.identity.id;
        auto collided = catalog.add(std::span{&collision, 1});
        require(!collided && collided.error() == EFlowNodeCatalogError::HASH_COLLISION);

        auto missing_codec = registration();
        missing_codec.identity = {graph::nodeTypeId("test.no_codec"), "test.no_codec", 1};
        missing_codec.encode = nullptr;
        missing_codec.decode = nullptr;
        require(catalog.add(std::span{&missing_codec, 1}).has_value());
        auto no_codec = catalog.find(missing_codec.identity.id);
        auto unencodable = no_codec->create();
        require(unencodable.has_value());
        auto unencodable_node = createFlowValueNode(no_codec, std::move(*unencodable));
        require(unencodable_node.has_value());
        FlowGraph unsaved;
        require(unsaved.addNode(std::move(*unencodable_node)).valid());
        auto rejected = captureFlowSource(assetId(), "unencodable", unsaved);
        require(!rejected && rejected.error().code == EFlowSourceError::NODE_CODEC_FAILURE);
        require(rejected.error().cause && rejected.error().cause->message == "node definition has no source codec");
    }

    void legacyRoundtrip(const std::filesystem::path& directory)
    {
        std::size_t count{};
        for (const auto& path : std::filesystem::directory_iterator(directory))
        {
            if (path.path().extension() != ".luxflow")
            {
                continue;
            }
            std::ifstream stream(path.path(), std::ios::binary);
            const std::string old{std::istreambuf_iterator<char>(stream), {}};
            require(old.find("version = 1") != std::string::npos);
            auto source = decodeFlowSource(old);
            require(source.has_value());
            auto graph = materializeFlowSource(*source);
            require(graph.has_value());
            for (const auto& node : source->nodes)
            {
                require(graph->topology().findNode(node.id)->type == graph::nodeTypeId(node.type));
            }
            auto captured = captureFlowSource(source->id, source->name, *graph);
            require(captured.has_value() && *captured == *source);
            auto bytes = encodeFlowSource(*captured);
            require(bytes.has_value() && bytes->find("version = 2") != std::string::npos);
            require(bytes->find("operation =") == std::string::npos);
            auto decoded = decodeFlowSource(*bytes);
            require(decoded.has_value() && *decoded == *source);
            auto restored = materializeFlowSource(*decoded);
            require(restored.has_value());
            require(*encodeFlowSource(*captureFlowSource(source->id, source->name, *restored)) == *bytes);
            ++count;
        }
        require(count == 33);
        std::puts("PASS 33 actual SDK v1 files -> canonical v2 -> graph -> identical v2");
    }
} // namespace

int main(int argc, char** argv)
{
    require(argc == 2);
    meta::meta_module_init();
    registeredRoundtrip();
    legacyRoundtrip(argv[1]);
    meta::meta_module_deinit();
    std::puts(
        "PASS registered source: binary codec, dynamic semantics, draft, missing definition/version/schema rejection"
    );
}
