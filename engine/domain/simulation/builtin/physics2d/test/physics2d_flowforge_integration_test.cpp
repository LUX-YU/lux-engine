#include "Physics2DFlowForgeTestAbility.hpp"
#include "Physics2DFlowForgeTestAbility.ability.generated.hpp"
#include "Physics2DFlowForgeTestAbility.ability.native.generated.hpp"
#include "PhysicsQuery2D.ability.generated.hpp"
#include "PhysicsQuery2D.ability.native.generated.hpp"
#include "Physics2DScriptTestSupport.hpp"

#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/function/script/native/NativeModule.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/scripting/native/NativeScriptBackend.hpp>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace
{
    using namespace lux;
    using namespace lux::physics2d;
    using namespace lux::physics2d::test;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    [[nodiscard]] asset::AssetId assetId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.front() = 0x2DU;
        return asset::AssetId{bytes};
    }

    struct CaptureProvider final
    {
        bool value{};
        std::size_t calls{};

        void capture(bool next) noexcept
        {
            value = next;
            ++calls;
        }
    };

    [[nodiscard]] flowforge::FlowGraph makeGraph(const flowforge::ScriptAbilityNodeDescription& physics,
                                                 const flowforge::ScriptAbilityNodeDescription& capture)
    {
        flowforge::FlowGraph graph;
        auto entry = std::make_unique<flowforge::OnEventNode>("tick");
        auto overlap = std::make_unique<flowforge::ScriptAbilityNode>(physics);
        auto record = std::make_unique<flowforge::ScriptAbilityNode>(capture);
        for (auto& parameter : overlap->parameterPins())
            assert(parameter->setConstantData(meta::RuntimeObject(double{0.25})));
        assert(overlap->parameterPins()[0]->setConstantData(meta::RuntimeObject(double{0.0})));
        assert(overlap->parameterPins()[1]->setConstantData(meta::RuntimeObject(double{0.0})));
        auto* entry_pointer = entry.get();
        auto* overlap_pointer = overlap.get();
        auto* record_pointer = record.get();
        const auto entry_slot = graph.addNodes(std::move(entry));
        graph.addNodes(std::move(overlap));
        graph.addNodes(std::move(record));
        flowforge::LastLink previous;
        assert(entry_pointer->execOutPin().linkTo(&overlap_pointer->execInPin(), previous) ==
               flowforge::ELinkError::SUCCESS);
        assert(overlap_pointer->execOutPin().linkTo(&record_pointer->execInPin(), previous) ==
               flowforge::ELinkError::SUCCESS);
        assert(const_cast<flowforge::DataOutPin&>(*overlap_pointer->resultPins().front())
                   .linkTo(record_pointer->parameterPins().front().get(), previous) == flowforge::ELinkError::SUCCESS);
        assert(
            graph.addExport({flowforge::FlowForgeExportNodeId{1U}, graph.getNode(entry_slot).node->id(), TickSymbol}));
        return graph;
    }

    struct Source final
    {
        const lux::script::ScriptArtifact* artifact{};
        const lux::script::NativeModule* module{};

        static bool resolveArtifact(void* context,
                                    const asset::AssetId& requested,
                                    ResolvedScriptArtifact& output) noexcept
        {
            const auto& self = *static_cast<Source*>(context);
            if (requested != assetId())
                return false;
            output.artifact = self.artifact;
            return true;
        }

        static bool resolveModule(void* context,
                                  const asset::AssetId& requested,
                                  const lux::script::ScriptArtifact&,
                                  ResolvedNativeModule& output) noexcept
        {
            const auto& self = *static_cast<Source*>(context);
            if (requested != assetId())
                return false;
            output.module = self.module;
            return true;
        }
    };
}

int main()
{
    using namespace lux;
    using namespace lux::physics2d;
    using namespace lux::physics2d::test;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    flowforge::ScriptAbilityNodeCatalog catalog;
    assert(catalog.add(flowforge::makeScriptAbilityCatalogContribution<PhysicsQuery2D>()));
    assert(catalog.add(flowforge::makeScriptAbilityCatalogContribution<Physics2DCaptureAbility>()));
    const auto* physics = catalog.view().find(lux::script::ScriptAbilityTraits<PhysicsQuery2D>::Description.id,
                                              lux::script::ScriptAbilityTraits<PhysicsQuery2D>::Methods.front().id);
    const auto* capture =
        catalog.view().find(lux::script::ScriptAbilityTraits<Physics2DCaptureAbility>::Description.id,
                            lux::script::ScriptAbilityTraits<Physics2DCaptureAbility>::Methods.front().id);
    assert(physics != nullptr && capture != nullptr);
    auto graph = makeGraph(*physics, *capture);
    auto artifact = flowforge::compileFlowForgeScript(
        graph,
        {.module_name = "lux.physics2d.flowforge-test", .script_abilities = catalog.view()});
    if (!artifact)
    {
        std::fprintf(stderr, "FlowForge compile failed: %s\n", artifact.error().message.c_str());
        return 1;
    }
    assert(artifact->description().api_requirements.size() == 2U);
    auto module = lux::script::loadNativeModule(artifact->payload(), artifact->description().module_name);
    assert(module);

    ecs::Registry registry;
    const auto collider = registry.create();
    registry.emplace<ecs::Transform2D>(collider);
    registry.emplace<BoxCollider2D>(collider);
    auto simulation = createSimulation(registry);
    assert(simulation);
    auto executor = task::TaskExecutor::create({0U, 1U});
    assert(executor);

    CaptureProvider capture_provider;
    const auto capture_binding = lux::script::bindScriptAbility<Physics2DCaptureAbility>(capture_provider);
    std::vector<ScriptApiCapabilityPublication> capabilities(simulation->scriptApiCapabilities().begin(),
                                                             simulation->scriptApiCapabilities().end());
    capabilities.push_back(publishScriptAbility(capture_binding));

    ScriptSystemDescriptionBuilder description_builder;
    assert(description_builder.addMount({ScriptMountId{1U},
                                         assetId(),
                                         SimulationScriptMount{},
                                         true,
                                         {{TickSymbol, HookScriptTarget{ProbeSystemId, TickHook}}}}));
    auto script_description = std::move(description_builder).build(simulation->description());
    assert(script_description);
    Source source{std::addressof(*artifact), std::addressof(*module)};
    const std::array native_contributions{
        lux::script::native::makeScriptAbilityNativeContribution<PhysicsQuery2D>(),
        lux::script::native::makeScriptAbilityNativeContribution<Physics2DCaptureAbility>()
    };
    NativeScriptBackend backend{{std::addressof(source), &Source::resolveModule},
                                {.module_capacity = 1U,
                                 .instance_capacity = 1U,
                                 .prepared_call_capacity = 2U,
                                 .continuation_capacity = 1U,
                                 .max_ability_imports_per_module = 2U,
                                 .max_continuation_frame_bytes = 256U,
                                 .continuation_frame_storage_bytes =
                                     2U * (256U) + 4096U,
                                 .max_event_wait_imports_per_module = 1U,
                                 .abilities = native_contributions,
                                 .storage_populations = std::array{
                                     lux::simulation::script::NativeScriptStoragePopulation{
                                         std::addressof(*module), 1U, 1U
                                     }
                                 },
                                 .state_storage_bytes = 64U * 1024U * 1024U}};
    assert(backend);
    const auto descriptor = backend.descriptor();
    auto system = ScriptSystem::create(simulation->description(),
                                       *script_description,
                                       registry,
                                       simulation->clock(),
                                       {8U, 1U, 1U, 1U, 1U, 1U, 64U, 1U, 1U, 1U, 1U, 1U},
                                       {std::addressof(source), &Source::resolveArtifact},
                                       {},
                                       capabilities,
                                       std::span{&descriptor, 1U},
                                       simulation->scriptHookEndpoints(),
                                       simulation->scriptEventEndpoints());
    assert(system && system->prepare());
    auto connection = bindScriptRuntime(*simulation, *system);
    assert(connection && simulation->execute(*executor, SimulationDuration{}));
    assert(capture_provider.calls == 1U && capture_provider.value);
    assert(system->activeContinuationCount() == 0U);
    assert(system->activeAwaitableCount() == 0U);
    assert(system->shutdown());
    return 0;
}
