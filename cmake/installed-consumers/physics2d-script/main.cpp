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
    if (!encoded)
        return 1;
    constexpr system::SystemInstanceId physics_id{71U};
    SimulationDescriptionBuilder description_builder;
    if (!description_builder.addSystem(physics_id, "physics2d", physics2DSystemDescription(), *encoded))
        return 2;
    auto description = std::move(description_builder).build();
    if (!description)
        return 3;
    SimulationSystemRegistry registrations;
    if (!registrations.add(physics2DSystemRegistrations()))
        return 4;
    ecs::Registry registry;
    const auto collider = registry.create();
    registry.emplace<ecs::Transform2D>(collider);
    registry.emplace<BoxCollider2D>(collider);
    auto simulation =
        Simulation::create(registry, std::make_shared<SimulationDescription>(std::move(*description)), registrations);
    if (!simulation)
        return 5;
    auto executor = task::TaskExecutor::create({0U, 1U});
    if (!executor || !simulation->execute(*executor, SimulationDuration{}))
        return 6;
    if (simulation->scriptApiCapabilities().empty())
        return 7;
    const auto capability = simulation->scriptApiCapabilities().front();
    using Traits = lux::script::ScriptAbilityTraits<PhysicsQuery2D>;
    const lux::script::ScriptAbilityBinding binding{
        &Traits::Description,
        capability.context,
        capability.dispatch,
        capability.methods
    };
    const auto prepared = lux::script::ScriptAbilityCpp<PhysicsQuery2D>::create(binding);
    if (!prepared || !prepared->overlapsBox(0.0, 0.0, 0.25, 0.25))
        return 8;

    flowforge::ScriptAbilityNodeCatalog catalog;
    if (!catalog.add(flowforge::makeScriptAbilityCatalogContribution<PhysicsQuery2D>()))
        return 9;
    const auto* query = catalog.view().find(Traits::Description.id, Traits::Methods.front().id);
    if (query == nullptr)
        return 10;
    flowforge::FlowGraph graph;
    auto entry = std::make_unique<flowforge::OnEventNode>("tick");
    auto overlap = std::make_unique<flowforge::ScriptAbilityNode>(*query);
    for (auto& parameter : overlap->parameterPins())
    {
        if (!parameter->setConstantData(meta::RuntimeObject(double{0.25})))
            return 11;
    }
    auto* entry_pointer = entry.get();
    auto* overlap_pointer = overlap.get();
    const auto entry_slot = graph.addNodes(std::move(entry));
    graph.addNodes(std::move(overlap));
    flowforge::LastLink previous;
    if (entry_pointer->execOutPin().linkTo(&overlap_pointer->execInPin(), previous) != flowforge::ELinkError::SUCCESS)
        return 12;
    if (!graph.addExport({flowforge::FlowForgeExportNodeId{1U}, graph.getNode(entry_slot).node->id(), 1U}))
        return 13;
    const auto flow_artifact = flowforge::compileFlowForgeScript(
        graph,
        {.module_name = "consumer.physics2d.flowforge", .script_abilities = catalog.view()});
    if (!flow_artifact || flow_artifact->description().api_requirements.size() != 1U)
        return 14;

    std::ifstream input(LUX_PHYSICS2D_LUA_ARTIFACT, std::ios::binary);
    if (!input)
        return 15;
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < std::streamoff{56})
        return 16;
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input)
        return 17;
    std::array<std::uint8_t, 16U> id_bytes{};
    for (std::size_t index{}; index < id_bytes.size(); ++index)
        id_bytes[index] = std::to_integer<std::uint8_t>(bytes[40U + index]);
    const auto lua_artifact = asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(
        asset::AssetId{id_bytes},
        lux::cxx::SharedBytes<>::copyOf(bytes),
        {bytes.size(), std::numeric_limits<std::size_t>::max(), 0U});
    if (!lua_artifact || (*lua_artifact)->data().description().api_requirements.size() != 1U)
        return 18;
    if ((*lua_artifact)->data().description().api_requirements.front().contract.name() != Traits::Description.id.name())
        return 19;
    return 0;
}
