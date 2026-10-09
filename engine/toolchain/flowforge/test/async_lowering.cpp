#include "FlowGraphFixture.hpp"
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityPayload.hpp>
#include <lux/engine/flowforge/script/ScriptEventPayload.hpp>

#include <fstream>

using namespace lux;
using namespace lux::flowforge;
using test::require;

template <class Payload, class Description>
NodeId addScript(FlowGraph& graph, const FlowNodeRegistration& registration, const Description& description)
{
    FlowNodeCatalog catalog;
    require(catalog.add({&registration, 1}).has_value());
    auto payload = FlowNodePayload::make<Payload, test::clone<Payload>>(object::CodeLease::builtin(), description);
    require(payload.has_value());
    auto node = createFlowNode(catalog.find(registration.identity.id), std::move(*payload));
    require(node.has_value());
    auto result = graph.addNode(std::move(*node));
    require(result.has_value());
    return *result;
}

int main(int argc, char** argv)
{
    require(argc <= 2);
    std::ofstream out;
    if (argc == 2)
    {
        out.open(argv[1], std::ios::binary);
        require(bool(out));
    }
    for (int mode = 0; mode != 3; ++mode)
    {
        FlowGraph graph;
        test::GraphFixture fixture;
        const auto entry = fixture.add(graph, "lux.flow.event", EventEntryPayload{}, "Tick");
        require(graph.addExport({{1}, entry, 41, {}}));
        ScriptAbilityNodeDescription ability{
            .contract = script::ScriptApiContractIdView{"test.analysis"},
            .method = script::ScriptApiMethodIdView{"wait"},
            .schema_hash = 73,
            .kind = script::EScriptApiMethodKind::ASYNC_OPERATION
        };
        script::ScriptEventSourceDescription event{
            .system_name = "test.simulation",
            .event_name = "arrived",
            .system_id = 7,
            .event_id = 9,
            .payload =
                {"lux.i32", semantic::typeId("lux.i32"), static_cast<std::uint8_t>(semantic::EAbiKind::I32), 4, 4},
            .payload_schema_hash = 11,
            .payload_schema_version = 1,
            .delivery_hook_id = 13,
            .delivery_schema_hash = 15,
            .delivery_schema_version = 1
        };
        FlowForgeCompileOptions options{
            .module_name = "analysis_async",
            .script_abilities = ScriptAbilityNodeCatalogView{{&ability, 1}},
            .script_events = {&event, 1}
        };
        if (mode == 0)
        {
            const auto call = addScript<ScriptAbilityPayload>(graph, scriptAbilityRegistration(), ability);
            test::link(graph, test::execOut(graph, entry), test::execIn(graph, call));
        }
        else if (mode == 1)
        {
            const auto function = fixture.add(graph, "lux.flow.function", FunctionPayload{}, "Subroutine");
            const auto call = fixture.add(graph, "lux.flow.function_call", FunctionCallPayload{function, {}, {}});
            test::link(graph, test::execOut(graph, entry), test::execIn(graph, call));
            const auto suspend = addScript<ScriptAbilityPayload>(graph, scriptAbilityRegistration(), ability);
            test::link(graph, test::execOut(graph, function), test::execIn(graph, suspend));
            const auto ret = fixture.add(graph, "lux.flow.function_return", FunctionReturnPayload{function, {}});
            test::link(graph, test::execOut(graph, suspend), test::execIn(graph, ret));
        }
        else
        {
            const auto wait = addScript<ScriptEventPayload>(graph, scriptEventRegistration(), event);
            test::link(graph, test::execOut(graph, entry), test::execIn(graph, wait));
        }
        auto compiled = compileFlowForgeObject(graph, options);
        if (!compiled)
        {
            std::fprintf(
                stderr,
                "mode%d error%u node%llu pin%llu %s\n",
                mode,
                unsigned(compiled.error().code),
                static_cast<unsigned long long>(compiled.error().node_id),
                static_cast<unsigned long long>(compiled.error().pin_id),
                compiled.error().message.c_str()
            );
            return 42;
        }
        require(!compiled->object.empty());
        require(compiled->description.exports.size() == 1);
        require(compiled->description.api_requirements.size() == (mode == 2 ? 0 : 1));
        require(compiled->description.event_requirements.size() == (mode == 2 ? 1 : 0));
        auto size = static_cast<std::uint64_t>(compiled->object.size());
        if (argc == 2)
        {
            out.write(reinterpret_cast<const char*>(&size), sizeof(size));
            out.write(reinterpret_cast<const char*>(compiled->object.data()), size);
            require(bool(out));
        }
        std::printf(
            "mode%d object%llu abilities%zu events%zu\n",
            mode,
            static_cast<unsigned long long>(size),
            compiled->description.api_requirements.size(),
            compiled->description.event_requirements.size()
        );
    }
}
