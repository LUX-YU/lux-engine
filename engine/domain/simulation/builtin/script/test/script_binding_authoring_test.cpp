#include "Behavior.AuthoringBehavior.script.generated.hpp"
#include <lux/engine/simulation/ScriptBindingAuthoring.hpp>
#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystemDescriptionCodec.hpp>

#include <array>
#include <cassert>

int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    using namespace lux::simulation::script;
    constexpr lux::system::SystemInstanceId owner{1U};
    constexpr std::array hooks{makeHookPointSpec<void(const std::int32_t&)>(HookPointId{2U}, "first"),
                               makeHookPointSpec<void(const std::int32_t&)>(HookPointId{3U}, "second"),
                               makeHookPointSpec<void(float)>(HookPointId{4U}, "wrong"),
                               makeHookPointSpec<void(const std::int32_t&)>(HookPointId{5U}, "native", false)};
    constexpr std::array events{makeEventPointSpec<std::int32_t>(EventPointId{6U}, "targeted", HookPointId{2U},
                                                                 EEventRoute::ENTITY_TARGETED, "lux.i32", 1U)};
    const SimulationSystemDescription host{
        .type = {.canonical_name = "authoring.Host", .version = 1U}, .hooks = hooks, .events = events};
    SimulationDescriptionBuilder builder;
    assert(builder.addSystem(owner, "host", host));
    auto simulation = std::move(builder).build();
    assert(simulation);
    auto description = materializeCppStaticScript(generated::AuthoringBehavior);
    assert(description);
    auto artifact = lux::script::ScriptArtifact::create(std::move(*description), {});
    assert(artifact);
    auto candidates = listScriptBindingCandidates(*artifact, 2U, *simulation, true);
    assert(candidates && candidates->size() == 5U);
    const ScriptBindingTarget first = HookScriptTarget{owner, HookPointId{2U}};
    const ScriptBindingTarget second = HookScriptTarget{owner, HookPointId{3U}};
    const std::array suggestions{first, second};
    const auto ambiguous = selectScriptBinding(2U, *candidates, nullptr, suggestions);
    assert(!ambiguous && ambiguous.error() == EScriptBindingAuthoringError::AMBIGUOUS_DEFAULT);
    const auto explicit_binding = selectScriptBinding(2U, *candidates, &second, suggestions);
    assert(explicit_binding && explicit_binding->target == second);
    assert(!selectScriptBinding(3U, *candidates, &second, {}));
    const ScriptBindingTarget wrong = HookScriptTarget{owner, HookPointId{4U}};
    assert(!selectScriptBinding(2U, *candidates, &wrong, {}));
    const auto simulation_scope = listScriptBindingCandidates(*artifact, 2U, *simulation, false);
    assert(simulation_scope && simulation_scope->back().compatibility == EScriptBindingCompatibility::SCOPE_MISMATCH);
    const auto lifecycle = listScriptBindingCandidates(*artifact, 1U, *simulation, true);
    assert(lifecycle && lifecycle->front().compatibility == EScriptBindingCompatibility::LIFECYCLE_ONLY);
    auto renamed_description = artifact->description();
    for (auto& method : renamed_description.exports)
        method.name = "renamed-diagnostic";
    auto renamed = lux::script::ScriptArtifact::create(std::move(renamed_description), {});
    assert(renamed);
    auto renamed_candidates = listScriptBindingCandidates(*renamed, 2U, *simulation, true);
    assert(renamed_candidates && selectScriptBinding(2U, *renamed_candidates, &second, {}));
    std::array<std::uint8_t, 16> identity{};
    identity[0] = 1U;
    ScriptSystemDescriptionBuilder mounts;
    assert(mounts.addMount({ScriptMountId{1U},
                            lux::asset::AssetId{identity},
                            EntityScriptMount{lux::world::WorldObjectId{uuids::uuid{identity}}},
                            true,
                            {*explicit_binding}}));
    auto authored = std::move(mounts).build(*simulation);
    assert(authored);
    constexpr ScriptSystemCodecLimits limits{65536U, 65536U, 65536U};
    auto bytes = encodeScriptSystemDescription(*authored, limits);
    assert(bytes);
    auto restored = decodeScriptSystemDescription(*bytes, *simulation, limits);
    assert(restored && restored->mounts().front().bindings.front() == *explicit_binding);
}
