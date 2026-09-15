#include <cassert>
#include <cstdio>
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/function/script/native/NativeModule.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <thread>
int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    lux::meta::ReflectionRegistry::initRegistry();
    auto runtime =
        lux::process::ExecutionRuntime::create({2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}});
    assert(runtime);
    const auto owner = std::this_thread::get_id();
    lux::flowforge::FlowGraph graph;
    const auto index = graph.addNodes(std::make_unique<lux::flowforge::OnEventNode>("tick"));
    const auto id = graph.getNode(index).node->id();
    constexpr lux::script::ScriptSymbolId symbol = 0x1234;
    assert(graph.addExport({lux::flowforge::FlowForgeExportNodeId{1}, id, symbol}));
    const lux::asset::AssetId asset(*uuids::uuid::from_string("00000000-0000-0000-0000-000000000003"));
    auto source = lux::flowforge::captureFlowSource(asset, "Robot Tick", graph);
    assert(source);
    auto encoded = lux::flowforge::encodeFlowSource(*source);
    assert(encoded);
    auto decoded = lux::flowforge::decodeFlowSource(*encoded);
    assert(decoded && *decoded == *source);
    auto reconstructed = lux::flowforge::materializeFlowSource(*decoded);
    assert(reconstructed && reconstructed->exports().front().symbol == symbol);
    auto duplicate = *source;
    duplicate.nodes.push_back(duplicate.nodes.front());
    auto rejected_source = lux::flowforge::validateFlowSource(duplicate);
    assert(!rejected_source && rejected_source.error().code == lux::flowforge::EFlowSourceError::INVALID_IDENTITY);
    auto wrong_parameters = *source;
    wrong_parameters.nodes.front().parameters = lux::flowforge::FlowSourceType{"double"};
    assert(!lux::flowforge::validateFlowSource(wrong_parameters));

    std::unique_ptr<lux::flowforge::Node> input = std::make_unique<lux::flowforge::BranchNode>(0);
    auto *original_node = input.get();
    std::unique_ptr<lux::flowforge::Node> *inserts[]{&input};
    lux::flowforge::FlowGraphChange insertion;
    insertion.insert = inserts;
    insertion.preserve_insert_ids = false;
    auto staged = lux::flowforge::FlowGraphEdit::prepare(graph, insertion);
    assert(staged && input.get() == original_node && graph.nodes().size() == 1);
    const auto inserted = staged->insertedIds().front();
    assert(staged->place(inserted, {20, 30, true}));
    staged->commit();
    assert(!input && graph.findNodeById(inserted) == original_node);
    const auto inserted_pin = original_node->inPins().front()->id();
    lux::flowforge::FlowGraphChange invalid;
    const lux::graph::LinkRecord wrong_link{inserted_pin, inserted_pin};
    invalid.connect = {&wrong_link, 1};
    const auto topology_size = graph.topology().links().size();
    assert(!lux::flowforge::FlowGraphEdit::prepare(graph, invalid));
    assert(graph.findNodeById(inserted) == original_node && graph.topology().links().size() == topology_size);
    lux::flowforge::FlowGraphChange removal;
    removal.erase = {&inserted, 1};
    auto detached = lux::flowforge::FlowGraphEdit::prepare(graph, removal);
    assert(detached && graph.findNodeById(inserted) == original_node);
    detached->commit();
    auto parked = detached->takeRemoved();
    assert(parked.size() == 1 && parked.front().get() == original_node && !original_node->graph());
    std::unique_ptr<lux::flowforge::Node> *restores[]{&parked.front()};
    lux::flowforge::FlowGraphChange restoration;
    restoration.insert = restores;
    auto restored = lux::flowforge::FlowGraphEdit::prepare(graph, restoration);
    assert(restored);
    restored->commit();
    assert(graph.findNodeById(inserted) == original_node && original_node->inPins().front()->id() == inserted_pin);
    auto cleanup = lux::flowforge::FlowGraphEdit::prepare(graph, removal);
    assert(cleanup);
    cleanup->commit();
    std::puts("source codec roundtrip and invalid parameter/identity rejection; atomic graph ownership and stable "
              "node/pin restoration passed");
    lux::flowforge::FlowForgeCompileOptions options;
    options.module_name = "d2.flow.object";
    options.lifecycle.begin_play = symbol;
    auto result = stdexec::sync_wait(stdexec::then(stdexec::schedule(runtime->cpu()),
                                                   [&]() noexcept
                                                   {
                                                       assert(std::this_thread::get_id() != owner);
                                                       return lux::flowforge::compileFlowForgeObject(graph, options);
                                                   }));
    assert(result);
    auto compiled = std::move(std::get<0>(*result));
    if (!compiled)
    {
        std::printf("compile=%u %s\n", unsigned(compiled.error().code), compiled.error().message.c_str());
    }
    assert(compiled && !compiled->object.empty());
    const auto original = compiled->object;
    auto failure = stdexec::sync_wait(stdexec::then(stdexec::schedule(*runtime->blocking()),
                                                    [&]() noexcept
                                                    {
                                                        assert(std::this_thread::get_id() != owner);
                                                        return lux::flowforge::linkFlowForgeObject(
                                                            *compiled, "missing-d2-test-linker.exe");
                                                    }));
    assert(failure);
    const auto &rejected = std::get<0>(*failure);
    assert(!rejected && rejected.error().code == lux::flowforge::EFlowForgeError::LINK_FAILED);
    assert(compiled->object == original && graph.nodes().size() == 1 && graph.exports().size() == 1);
    std::printf("link rejection=%u; retained object=%zu bytes\n", unsigned(rejected.error().code), original.size());
    auto link =
        stdexec::sync_wait(stdexec::then(stdexec::schedule(*runtime->blocking()),
                                         [&]() noexcept
                                         {
                                             assert(std::this_thread::get_id() != owner);
                                             return lux::flowforge::linkFlowForgeObject(*compiled, D2_FLOW_LINKER);
                                         }));
    assert(link);
    auto artifact = std::move(std::get<0>(*link));
    if (!artifact)
    {
        std::printf("link=%u %s\n", unsigned(artifact.error().code), artifact.error().message.c_str());
    }
    assert(artifact && compiled->object == original);
    auto module = lux::script::loadNativeModule(artifact->payload(), "d2.flow.object");
    assert(module && module->findFunction(symbol));
    lux_script_call_frame frame{};
    lux_script_native_instance_context instance{};
    assert(module->findFunction(symbol)->invoke(&instance, &frame) == 0);
    std::printf("loaded actual linked DLL=%zu bytes; native Tick invocation succeeded\n", artifact->payload().size());
    runtime->requestStop();
    assert(runtime->join());
    std::puts("PASS CPU FlowGraph -> COFF; Blocking linker failure preserves input; same-object retry -> native DLL -> "
              "actual ABI invocation");
}
