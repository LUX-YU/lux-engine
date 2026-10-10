#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityPayload.hpp>

#include <algorithm>
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
        result.capture_source = [](const FlowNodePayload& value) noexcept -> FlowSourceResult<VFlowSourceParameters>
        {
            const auto& payload = *value.get<Payload>();
            return FlowSourcePayload{
                std::string{static_cast<char>(payload.outputs), static_cast<char>(payload.reject), '\0', '\xff'}
            };
        };
        result.restore_source = [](const FlowSourceNode& source,
                                   const FlowSourceEnvironment&,
                                   FlowReferenceView,
                                   const object::CodeLease& code) noexcept -> FlowSourceResult<FlowNodePayload>
        {
            const auto* saved = std::get_if<FlowSourcePayload>(&source.parameters);
            if (!saved)
            {
                return cxx::unexpected(FlowSourceFailure{EFlowSourceError::SCHEMA_MISMATCH});
            }
            const std::string_view bytes = saved->bytes;
            const bool has_size = bytes.size() == 4;
            const bool has_schema = has_size && bytes[0] > 0 && bytes[0] <= 4 && (bytes[1] == 0 || bytes[1] == 1) &&
                                    bytes[2] == 0 && static_cast<unsigned char>(bytes[3]) == 255;
            if (!has_schema)
            {
                return cxx::unexpected(FlowSourceFailure{
                    EFlowSourceError::NODE_CODEC_FAILURE,
                    {},
                    {},
                    {},
                    0,
                    0,
                    FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "invalid source payload"}
                });
            }
            auto result = FlowNodePayload::make<Payload, clone>(code, static_cast<unsigned>(bytes[0]), bytes[1] != 0);
            require(result.has_value());
            return std::move(*result);
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
        auto node = createFlowNode(definition, std::move(*payload));
        require(node.has_value());
        const auto id = graph.addNode(std::move(*node));
        require(id.has_value());
        const graph::GraphLayoutEntry placement{*id, {12.0F, -3.0F, true}};
        FlowGraphChange layout;
        layout.place = std::span{&placement, 1};
        auto edit = FlowGraphEdit::prepare(graph, layout);
        require(edit.has_value());
        edit->commit();
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
        const auto* actual = restored->node(*id);
        require(actual && std::ranges::count(restored->topology().pins(), *id, &graph::PinRecord::owner) == 4);
        require(actual->payload.get<Payload>()->reject);
        require(!definition->validate(actual->payload));
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
        missing_codec.capture_source = nullptr;
        missing_codec.restore_source = nullptr;
        require(catalog.add(std::span{&missing_codec, 1}).has_value());
        auto no_codec = catalog.find(missing_codec.identity.id);
        auto unencodable = no_codec->create();
        require(unencodable.has_value());
        auto unencodable_node = createFlowNode(no_codec, std::move(*unencodable));
        require(unencodable_node.has_value());
        FlowGraph unsaved;
        require(unsaved.addNode(std::move(*unencodable_node)).has_value());
        auto rejected = captureFlowSource(assetId(), "unencodable", unsaved);
        require(!rejected && rejected.error().code == EFlowSourceError::NODE_CODEC_FAILURE);
        require(rejected.error().cause && rejected.error().cause->message == "node definition has no source codec");
    }

    unsigned restored_declarations{};
    unsigned restored_bodies{};

    void registeredMetadataInputs()
    {
        FlowNodeCatalog catalog;
        auto declaration = registration();
        declaration.identity = {graph::nodeTypeId("test.source_declaration"), "test.source_declaration", 1};
        declaration.source_stage = EFlowSourceStage::DECLARATION;
        declaration.restore_source = [](const FlowSourceNode& source,
                                        const FlowSourceEnvironment& environment,
                                        FlowReferenceView references,
                                        const object::CodeLease& code) noexcept -> FlowSourceResult<FlowNodePayload>
        {
            ++restored_declarations;
            return registration().restore_source(source, environment, references, code);
        };
        auto body = registration();
        body.identity = {graph::nodeTypeId("test.source_body"), "test.source_body", 1};
        body.restore_source = [](const FlowSourceNode& source,
                                 const FlowSourceEnvironment& environment,
                                 FlowReferenceView references,
                                 const object::CodeLease& code) noexcept -> FlowSourceResult<FlowNodePayload>
        {
            ++restored_bodies;
            const bool has_metadata =
                environment.types.size() == 1 && environment.types.front() == &meta::ref_type_of_v<int>;
            if (!has_metadata)
            {
                return cxx::unexpected(FlowSourceFailure{EFlowSourceError::UNKNOWN_TYPE, "external metadata", source.id}
                );
            }
            const auto* declared = references.node(NodeId{99});
            const bool has_declaration =
                declared && declared->definition->sourceStage() == EFlowSourceStage::DECLARATION;
            const bool has_variable = references.variable_type(7) == environment.types.front();
            if (!has_declaration || !has_variable)
            {
                return cxx::unexpected(
                    FlowSourceFailure{EFlowSourceError::INVALID_IDENTITY, "external reference", source.id}
                );
            }
            return registration().restore_source(source, environment, references, code);
        };
        const std::array entries{declaration, body};
        require(catalog.add(entries).has_value());
        auto invalid = registration();
        invalid.restore_source = nullptr;
        require(!catalog.add(std::span{&invalid, 1}));
        invalid = registration();
        invalid.source_stage = static_cast<EFlowSourceStage>(255);
        require(!catalog.add(std::span{&invalid, 1}));

        FlowGraph graph;
        require(graph.addVariableWithId(7, "state", &meta::ref_type_of_v<int>, meta::RuntimeObject{12}));
        for (const auto& entry : entries)
        {
            auto definition = catalog.find(entry.identity.id);
            auto payload = definition->create();
            require(payload.has_value());
            auto node = createFlowNode(definition, std::move(*payload));
            require(node.has_value());
            const NodeId id{entry.source_stage == EFlowSourceStage::DECLARATION ? 99U : 1U};
            require(graph.addNodeWithId(id, std::move(*node)).has_value());
        }
        const auto captured = captureFlowSource(assetId(), "registered metadata", graph);
        require(captured.has_value() && captured->nodes.front().id == NodeId{1});
        const auto bytes = encodeFlowSource(*captured);
        require(bytes.has_value());
        const std::array types{&meta::ref_type_of_v<int>};
        FlowSourceEnvironment environment;
        environment.nodes = &catalog;
        environment.types = types;
        auto restored = materializeFlowSource(*captured, environment);
        require(restored.has_value() && restored_declarations == 1 && restored_bodies == 1);
        require(*captureFlowSource(assetId(), "registered metadata", *restored) == *captured);
        require(*encodeFlowSource(*captureFlowSource(assetId(), "registered metadata", *restored)) == *bytes);
        environment.types = {};
        const auto refused = materializeFlowSource(*captured, environment);
        require(!refused && refused.error().code == EFlowSourceError::UNKNOWN_TYPE);
        require(refused.error().field == "external metadata" && refused.error().node == NodeId{1});
        require(*captureFlowSource(assetId(), "registered metadata", graph) == *captured);
        auto wrong_version = captured->nodes.front();
        wrong_version.version = 2;
        const auto find_node = [](NodeId) noexcept -> const FlowNode* { return nullptr; };
        const auto find_variable = [](std::uint64_t) noexcept -> const meta::RefType* { return nullptr; };
        const auto before = restored_bodies;
        const auto version_rejected =
            catalog.find(body.identity.id)->restoreSource(wrong_version, environment, {find_node, find_variable});
        require(!version_rejected && version_rejected.error().code == EFlowSourceError::SCHEMA_MISMATCH);
        require(restored_bodies == before);
        std::puts("PASS external source provider: metadata, staged forward reference, variable lookup, exact failure "
                  "and unchanged graph");
    }

    void functionForwardReferences()
    {
        FlowNodeCatalog catalog;
        require(catalog.add(functionNodeRegistrations()).has_value());
        const auto make = [&]<class T>(std::string_view name, T value) noexcept
        {
            auto definition = catalog.find(graph::nodeTypeId(name));
            auto payload = definition->create();
            require(payload.has_value());
            *payload->get<T>() = std::move(value);
            auto node = createFlowNode(definition, std::move(*payload));
            require(node.has_value());
            return std::move(*node);
        };
        const std::vector<FuncArgInfo> arguments{{&meta::ref_type_of_v<int>, "argument"}};
        const std::vector<FuncArgInfo> results{{&meta::ref_type_of_v<int>, "result"}};
        auto function = make("lux.flow.function", FunctionPayload{arguments, results});
        auto call = make("lux.flow.function_call", FunctionCallPayload{NodeId{99}, arguments, results});
        auto returned = make("lux.flow.function_return", FunctionReturnPayload{NodeId{99}, results});
        const std::array entries{
            FlowNodeEntry{NodeId{1}, &call},
            FlowNodeEntry{NodeId{2}, &returned},
            FlowNodeEntry{NodeId{99}, &function}
        };
        FlowGraph graph;
        auto edit = FlowGraphEdit::prepare(graph, {.insert = entries});
        require(edit.has_value());
        edit->commit();
        auto source = captureFlowSource(assetId(), "functions", graph);
        require(source.has_value());
        // Wire pin order is schema order, never ascending PinId. Include the final valid ID.
        auto next = UINT64_MAX;
        for (auto& node : source->nodes)
        {
            for (auto& pin : node.inputs)
            {
                pin.id = PinId{next--};
            }
            for (auto& pin : node.outputs)
            {
                pin.id = PinId{next--};
            }
        }
        auto restored = materializeFlowSource(*source);
        require(restored.has_value());
        auto captured = captureFlowSource(source->id, source->name, *restored);
        require(captured.has_value() && *captured == *source);
        const auto* actual = restored->node(NodeId{1})->payload.get<FunctionCallPayload>();
        require(actual && actual->callee == NodeId{99} && actual->arguments.size() == 1);
        auto invalid = *source;
        std::get<FlowSourceReference>(invalid.nodes.front().parameters).id = 200;
        const auto refused = materializeFlowSource(invalid);
        require(!refused && refused.error().code == EFlowSourceError::INVALID_IDENTITY);
        require(*captureFlowSource(source->id, source->name, *restored) == *source);
        std::puts("PASS function forward references, atomic reconstruction and non-ordinal/max PinIds");
    }

    void restoredMetadataLifetime()
    {
        const script::ScriptAbilityValueDescription type{
            semantic::typeId("lux.i32"),
            "lux.i32",
            semantic::EValuePass::VALUE,
            static_cast<std::uint8_t>(semantic::EAbiKind::I32),
            4,
            4,
            script::EScriptAbilityValueLifetime::OWNED_VALUE
        };
        const std::array parameters{script::ScriptAbilityParameterDescription{"value", type}};
        const ScriptAbilityNodeDescription description{
            script::ScriptApiContractIdView{"test.source"},
            script::ScriptApiMethodIdView{"call"},
            "Source",
            "Call",
            1,
            17,
            script::EScriptAbilityReceiverKind::NONE,
            script::EScriptApiMethodKind::QUERY,
            parameters,
            {}
        };
        FlowNodeCatalog catalog;
        const auto registration = scriptAbilityRegistration();
        require(catalog.add({&registration, 1}).has_value());
        auto definition = catalog.find(registration.identity.id);
        auto payload = definition->create();
        require(payload.has_value());
        *payload->get<ScriptAbilityPayload>() = ScriptAbilityPayload{description};
        auto value = createFlowNode(definition, std::move(*payload));
        require(value.has_value());
        FlowGraph graph;
        const auto id = graph.addNode(std::move(*value));
        require(id.has_value());
        const auto before = captureFlowSource(assetId(), "metadata", graph);
        require(before.has_value());
        {
            auto removed = graph.extractNode(*id);
            require(removed.has_value());
            const FlowNodeEntry entry{*id, &removed->value, removed->pins};
            auto edit = FlowGraphEdit::prepare(graph, {.insert = std::span{&entry, 1}});
            require(edit.has_value());
            edit->commit();
            const auto* node = graph.node(*id);
            const auto schema = node->definition->describePins(node->payload);
            require(schema.has_value());
            const auto& argument = (*schema)[1];
            const auto* pin = graph.pin(graph.pinId(*id, argument.semantic));
            // No dangling dereference: check while both old/new metadata owners still exist.
            require(pin->type == argument.type);
            require(pin->default_value.type() == argument.type);
        }
        auto captured = captureFlowSource(assetId(), "metadata", graph);
        require(captured.has_value() && *captured == *before);
        ScriptAbilityNodeCatalog abilities;
        require(abilities.add({{&description, 1}}).has_value());
        FlowSourceEnvironment environment;
        environment.abilities = abilities.view();
        auto restored = materializeFlowSource(*captured, environment);
        require(restored.has_value());
        graph = {};
        auto again = captureFlowSource(assetId(), "metadata", *restored);
        require(again.has_value() && *again == *before);
        std::puts("PASS restored Script metadata/defaults survive original node and temporary source owners");
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
    registeredMetadataInputs();
    functionForwardReferences();
    restoredMetadataLifetime();
    legacyRoundtrip(argv[1]);
    meta::meta_module_deinit();
    std::puts(
        "PASS registered source: binary codec, dynamic semantics, draft, missing definition/version/schema rejection"
    );
}
