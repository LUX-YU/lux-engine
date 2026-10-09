#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/material/BuiltinMaterialNodes.hpp>
#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/MaterialNodeCatalog.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>

#include "node_plugin_state.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace
{
    void require(bool condition) noexcept
    {
        if (!condition)
        {
            std::abort();
        }
    }
} // namespace

int main(int argc, char** argv)
{
    require(argc == 2);
    using namespace lux;
    using namespace lux::material;
    using Library = engine::platform::DynamicLibrary;
    NodePluginState state;
    std::shared_ptr<const MaterialNodeType> type;
    MaterialNodePayload payload;
    std::weak_ptr<Library> observed_library;
    {
        auto library = std::shared_ptr<Library>(
            new Library(std::filesystem::path{argv[1]}),
            [&](Library* value) noexcept
            {
                require(state.destroyed == state.constructed);
                delete value;
                state.unloaded = true;
            }
        );
        require(library->is_loaded());
        observed_library = library;
        using Entry = void(MaterialNodeRegistration&, const object::CodeLease&, NodePluginState&) noexcept;
        const auto entry = library->get_symbol<Entry>("materialNodeRegistration");
        require(entry != nullptr);
        MaterialNodeRegistration registration;
        entry(registration, object::CodeLease::plugin(library), state);
        MaterialNodeCatalog catalog;
        require(catalog.add({&registration, 1}).has_value());
        type = catalog.find(registration.identity.id);
        require(bool(type));
        auto result = type->create();
        require(result.has_value());
        payload = std::move(*result);
    }
    require(!observed_library.expired() && !state.unloaded);
    require(type->describePins(payload)->size() == 1);
    shadergen::ShaderIR ir;
    auto outputs = type->compile(payload, {}, ir);
    require(outputs.has_value() && outputs->size() == 1);
    require(ir.values[outputs->front()].constant[0] == 0.625F);
    type.reset();
    require(!observed_library.expired());
    state.reject_clone = true;
    auto rejection = payload.clone();
    require(!rejection && rejection.error().message == "DLL clone rejected");
    require(!observed_library.expired());
    state.reject_clone = false;
    auto copy = payload.clone();
    require(copy.has_value() && state.clone_returns == 2);
    payload = MaterialNodePayload{};
    require(state.destroyed == 1 && !state.unloaded);
    *copy = MaterialNodePayload{};
    require(state.destroyed == 2 && state.unloaded && observed_library.expired());
    // Exercise the same actual DLL through the graph, source codec, edit and compiler entry points.
    NodePluginState graph_state;
    MaterialSource source{asset::AssetId{std::array<std::uint8_t, 16>{1}}, "DLL graph", {}};
    std::optional<MaterialSource> decoded;
    std::weak_ptr<Library> graph_library;
    graph::NodeId external;
    {
        auto library = std::shared_ptr<Library>(
            new Library(std::filesystem::path{argv[1]}),
            [&](Library* value) noexcept
            {
                require(graph_state.destroyed == graph_state.constructed);
                delete value;
                graph_state.unloaded = true;
            }
        );
        require(library->is_loaded());
        graph_library = library;
        using Entry = void(MaterialNodeRegistration&, const object::CodeLease&, NodePluginState&) noexcept;
        const auto entry = library->get_symbol<Entry>("materialNodeRegistration");
        require(entry != nullptr);
        MaterialNodeRegistration registration;
        entry(registration, object::CodeLease::plugin(library), graph_state);
        MaterialNodeCatalog catalog;
        require(catalog.add({&registration, 1}).has_value());
        const auto builtins = materialBuiltinRegistrations();
        require(catalog.add(builtins).has_value());
        auto definition = catalog.find(registration.identity.id);
        auto value = definition->create();
        require(value.has_value());
        auto inserted = source.graph.addNode({definition, "external", std::move(*value)});
        require(inserted.has_value());
        external = *inserted;
        auto surface = catalog.find(graph::nodeTypeId(MaterialOutputSurface::TypeName));
        auto surface_payload = surface->create();
        require(surface_payload.has_value());
        auto output = source.graph.addNode({surface, "surface", std::move(*surface_payload)});
        require(output.has_value());
        require(source.graph
                    .connect(
                        source.graph.pinId(external, graph::PinSemanticId{1}),
                        source.graph.pinId(*output, graph::PinSemanticId{4})
                    )
                    .has_value());
        auto encoded = encodeMaterialSource(source);
        require(encoded.has_value());
        auto restored = decodeMaterialSource(*encoded, catalog);
        require(restored.has_value());
        require(*encodeMaterialSource(*restored) == *encoded);
        decoded.emplace(std::move(*restored));
    }
    require(!graph_library.expired() && !graph_state.unloaded);
    auto lowered = lowerMaterial(source.graph);
    require(lowered.has_value() && lowered->shader.values.front().constant[0] == 0.625F);
    auto bytes = encodeMaterialSource(source);
    require(bytes.has_value());
    graph_state.reject_clone = true;
    auto failed_copy = source.graph.clone();
    require(!failed_copy && std::holds_alternative<MaterialCompileFailure>(failed_copy.error()));
    require(std::get<MaterialCompileFailure>(failed_copy.error()).message == "DLL clone rejected");
    require(*encodeMaterialSource(source) == *bytes);
    graph_state.reject_clone = false;
    auto copied = source.graph.clone();
    require(copied.has_value());
    auto extracted = source.graph.extractNode(external);
    require(extracted.has_value());
    source.graph = MaterialGraph{};
    decoded.reset();
    *copied = MaterialGraph{};
    require(!graph_library.expired());
    extracted->value = MaterialNode{};
    require(graph_library.expired() && graph_state.unloaded);
    std::puts(
        "PASS: actual Material DLL code retained through catalog release, compilation, clone and final payload cleanup"
    );
}
