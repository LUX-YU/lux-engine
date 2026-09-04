#include <lux/engine/physics2d/Physics2DSystem.hpp>
#include <lux/engine/physics2d/abilities/PhysicsQuery2D.ability.generated.hpp>

#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/task/TaskExecutor.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

int main()
{
    using namespace lux;
    using namespace lux::physics2d;
    using namespace lux::simulation;

    Physics2DSystemConfiguration configuration;
    configuration.gravity_y = 0.0;
    configuration.body_capacity = 8U;
    const auto encoded = makePhysics2DSystemConfiguration(configuration);
    assert(encoded);
    constexpr system::SystemInstanceId physics_id{71U};
    SimulationDescriptionBuilder description_builder;
    assert(description_builder.addSystem(physics_id, "physics2d", physics2DSystemDescription(), *encoded));
    auto description = std::move(description_builder).build();
    assert(description);
    SimulationSystemRegistry registrations;
    assert(registrations.add(physics2DSystemRegistrations()));
    ecs::Registry registry;
    const auto collider = registry.create();
    registry.emplace<ecs::Transform2D>(collider);
    registry.emplace<BoxCollider2D>(collider);
    auto simulation =
        Simulation::create(registry, std::make_shared<SimulationDescription>(std::move(*description)), registrations);
    assert(simulation);
    auto executor = task::TaskExecutor::create({0U, 1U});
    assert(executor && simulation->execute(*executor, SimulationDuration{}));
    const auto capability = simulation->scriptApiCapabilities().front();
    using Traits = script::ScriptAbilityTraits<PhysicsQuery2D>;
    const script::ScriptAbilityBinding binding{&Traits::Description,
                                               capability.context,
                                               capability.dispatch,
                                               capability.methods};
    const auto prepared = script::ScriptAbilityCpp<PhysicsQuery2D>::create(binding);
    assert(prepared && prepared->overlapsBox(0.0, 0.0, 0.25, 0.25));

    flowforge::ScriptAbilityNodeCatalog catalog;
    assert(catalog.add(flowforge::makeScriptAbilityCatalogContribution<PhysicsQuery2D>()));
    const auto* query = catalog.view().find(Traits::Description.id, Traits::Methods.front().id);
    assert(query != nullptr);
    flowforge::FlowGraph graph;
    auto entry = std::make_unique<flowforge::OnEventNode>("tick");
    auto overlap = std::make_unique<flowforge::ScriptAbilityNode>(*query);
    for (auto& parameter : overlap->parameterPins())
        assert(parameter->setConstantData(meta::RuntimeObject(double{0.25})));
    auto* entry_pointer = entry.get();
    auto* overlap_pointer = overlap.get();
    const auto entry_slot = graph.addNodes(std::move(entry));
    graph.addNodes(std::move(overlap));
    flowforge::LastLink previous;
    assert(entry_pointer->execOutPin().linkTo(&overlap_pointer->execInPin(), previous) ==
           flowforge::ELinkError::SUCCESS);
    assert(graph.addExport({flowforge::FlowForgeExportNodeId{1U}, graph.getNode(entry_slot).node->id(), 1U}));
    const auto flow_artifact = flowforge::compileFlowForgeScript(
        graph,
        {.module_name = "consumer.physics2d.flowforge", .script_abilities = catalog.view()});
    assert(flow_artifact && flow_artifact->description().api_requirements.size() == 1U);

    std::ifstream input(LUX_PHYSICS2D_LUA_ARTIFACT, std::ios::binary);
    assert(input);
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    std::array<std::uint8_t, 16U> id_bytes{};
    for (std::size_t index{}; index < id_bytes.size(); ++index)
        id_bytes[index] = std::to_integer<std::uint8_t>(bytes[40U + index]);
    const auto lua_artifact = asset::TAssetSerDeser<script::ScriptArtifactAsset>::decode(
        asset::AssetId{id_bytes},
        lux::cxx::SharedBytes<>::copyOf(bytes),
        {bytes.size(), std::numeric_limits<std::size_t>::max(), 0U});
    assert(lua_artifact);
    assert((*lua_artifact)->data().description().api_requirements.size() == 1U);
    assert((*lua_artifact)->data().description().api_requirements.front().contract == Traits::Description.id);
    return 0;
}
