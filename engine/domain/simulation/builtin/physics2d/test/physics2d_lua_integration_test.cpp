#include "PhysicsQuery2D.ability.generated.hpp"
#include "PhysicsQuery2D.ability.lua.generated.hpp"
#include "Physics2DScriptTestSupport.hpp"

#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/task/TaskExecutor.hpp>

#include "DelayAbility.ability.generated.hpp"
#include "DelayAbility.ability.lua.generated.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace
{
    using namespace lux;
    using namespace lux::physics2d;
    using namespace lux::physics2d::test;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    [[nodiscard]] std::shared_ptr<const lux::script::ScriptArtifactAsset> artifactAsset()
    {
        std::ifstream input(LUX_PHYSICS2D_LUA_ARTIFACT, std::ios::binary);
        assert(input);
        input.seekg(0, std::ios::end);
        const auto encoded_size = input.tellg();
        assert(encoded_size >= std::streamoff{56});
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<std::size_t>(encoded_size));
        input.read(reinterpret_cast<char*>(bytes.data()), encoded_size);
        assert(input);
        std::array<std::uint8_t, 16U> id_bytes{};
        for (std::size_t index{}; index < id_bytes.size(); ++index)
            id_bytes[index] = std::to_integer<std::uint8_t>(bytes[40U + index]);
        const auto decoded = asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(
            asset::AssetId{id_bytes},
            lux::cxx::SharedBytes<>::copyOf(bytes),
            {bytes.size(), std::numeric_limits<std::size_t>::max(), 0U});
        assert(decoded);
        return std::move(*decoded);
    }

    struct Source final
    {
        std::shared_ptr<const lux::script::ScriptArtifactAsset> asset;

        static bool resolve(void* context, const asset::AssetId& requested, ResolvedScriptArtifact& output) noexcept
        {
            const auto& self = *static_cast<Source*>(context);
            if (!self.asset || requested != self.asset->id())
                return false;
            output.artifact = std::addressof(self.asset->data());
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

    auto artifact = artifactAsset();
    assert(artifact->data().description().event_requirements.size() == 1U);
    assert(artifact->data().description().api_requirements.size() == 2U);
    ecs::Registry registry;
    const auto collider = registry.create();
    registry.emplace<ecs::Transform2D>(collider);
    registry.emplace<BoxCollider2D>(collider);
    auto simulation = createSimulation(registry);
    assert(simulation);
    auto executor = task::TaskExecutor::create({0U, 1U});
    assert(executor);
    assert(simulation->execute(*executor, SimulationDuration{}));
    assert(simulation->scriptApiCapabilities().size() == 1U);
    auto* physics = static_cast<Physics2DSystem*>(simulation->scriptApiCapabilities().front().context);
    assert(physics != nullptr && physics->overlapsBox(0.0, 0.0, 0.25, 0.25));

    const std::array contributions{
        lux::script::lua::makeScriptAbilityLuaContribution<PhysicsQuery2D>(),
        lux::script::lua::makeScriptAbilityLuaContribution<DelayAbility>()
    };
    const std::array event_sources{pulseEventSource()};
    auto backend = LuaScriptBackend::create({.instance_capacity = 1U,
                                             .prepared_call_capacity = 5U,
                                             .continuation_capacity = 1U,
                                             .execution_depth_capacity = 4U,
                                             .ability_catalog_method_capacity = 5U,
                                             .prepared_ability_capacity = 5U,
                                             .abilities = contributions,
                                             .event_catalog_capacity = 1U,
                                             .prepared_event_capacity = 1U,
                                             .events = event_sources});
    assert(backend);
    const auto descriptor = backend->descriptor();
    ScriptSystemDescriptionBuilder description_builder;
    assert(description_builder.addMount({ScriptMountId{1U},
                                         artifact->id(),
                                         SimulationScriptMount{},
                                         true,
                                         {{TickSymbol, HookScriptTarget{ProbeSystemId, TickHook}}}}));
    auto script_description = std::move(description_builder).build(simulation->description());
    assert(script_description);
    Source source{std::move(artifact)};
    auto system = ScriptSystem::create(simulation->description(),
                                       *script_description,
                                       registry,
                                       simulation->clock(),
                                       {8U, 1U, 1U, 1U, 1U, 1U, 64U, 1U, 1U, 1U, 1U, 1U},
                                       {std::addressof(source), &Source::resolve},
                                       {},
                                       simulation->scriptApiCapabilities(),
                                       std::span{&descriptor, 1U},
                                       simulation->scriptHookEndpoints(),
                                       simulation->scriptEventEndpoints());
    assert(system && system->prepare());
    assert(ActiveProbe != nullptr && ActiveProbe->tick.dispatch() == 1U);
    assert(system->activeContinuationCount() == 1U);
    assert(system->activeAwaitableCount() == 1U);
    assert(system->stats().active_event_waiters == 1U);
    {
        auto writer = ActiveProbe->pulse.begin(0U);
        assert(writer.record(1));
    }
    assert(ActiveProbe->pulse.drain() == 1U);
    assert(system->activeContinuationCount() == 1U);
    assert(system->executeStablePoint());
    assert(system->activeContinuationCount() == 1U);
    assert(system->stats().next_step_waits == 1U);
    assert(simulation->execute(*executor, std::chrono::milliseconds{16}));
    assert(system->executeStablePoint());
    if (!system->failures().empty())
    {
        const auto& failure = system->failures().back();
        std::fprintf(stderr,
                     "Physics2D Lua invocation failed: error=%u status=%d\n",
                     static_cast<unsigned>(failure.error),
                     failure.status);
    }
    assert(system->failures().empty());
    assert(system->activeContinuationCount() == 0U);
    assert(system->activeAwaitableCount() == 0U);
    assert(system->shutdown());
    return 0;
}
