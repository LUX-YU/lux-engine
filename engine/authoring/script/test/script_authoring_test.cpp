#include <lux/engine/authoring/script/ScriptAuthoring.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>

#include <array>
#include <cassert>

namespace
{
    inline constexpr lux::simulation::HookPointId UpdateHook{11U};
    inline constexpr lux::simulation::HookPointId CollisionHook{12U};
    inline constexpr lux::simulation::EventPointId CollisionEvent{21U};
    inline constexpr lux::system::SystemInstanceId SystemInstance{31U};
    inline constexpr std::array Hooks{
        lux::simulation::makeHookPointSpec<void(
            const lux::simulation::SimulationStepInfo&)>(
                UpdateHook,
                "update"),
        lux::simulation::makeHookPointSpec<void()>(
            CollisionHook,
            "after-collision")};
    inline constexpr std::array Events{
        lux::simulation::makeEventPointSpec<std::int32_t>(
            CollisionEvent,
            "collision",
            CollisionHook,
            lux::simulation::EEventRoute::ENTITY_TARGETED,
            "lux.i32",
            1U)};
    inline constexpr lux::simulation::SimulationSystemDescription System{
        .type = {.canonical_name = "lux.test.authoring", .version = 1U},
        .hooks = Hooks,
        .events = Events};
}

int main()
{
    using namespace lux::authoring::script;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    SimulationDescriptionBuilder builder;
    assert(builder.addSystem(SystemInstance, "fixture", System));
    auto simulation = std::move(builder).build();
    assert(simulation);
    const auto system = simulation->systemAt(0U);

    lux::rdesc::Script asset;
    asset.module_name = "authoring.fixture";
    asset.body = lux::rdesc::CppStaticScript{"fixture"};
    asset.exports = {
        {"update", 1U, {
            lux::rdesc::makeScriptValueType<SimulationStepInfo>(
                lux::semantic::EValuePass::CONST_REF)}, {}},
        {"collision", 2U, {
            lux::rdesc::makeScriptValueType<std::int32_t>(
                lux::semantic::EValuePass::CONST_REF)}, {}}};
    assert(lux::rdesc::validScriptDescription(asset));

    const auto hooks = compatibleHookTargets(
        *simulation,
        asset.exports[0]);
    assert(hooks.size() == 1U);
    assert((hooks[0] == HookScriptTarget{system.instanceId(), UpdateHook}));
    const auto events = compatibleEventTargets(
        *simulation,
        asset.exports[1],
        false);
    assert(events.size() == 1U);

    std::array<std::uint8_t, 16U> asset_bytes{};
    asset_bytes[0] = 1U;
    std::array<std::uint8_t, 16U> object_bytes{};
    object_bytes[0] = 2U;
    ScriptMountDescription mount{
        ScriptMountId{1U},
        lux::asset::AssetId{asset_bytes},
        EntityScriptMount{
            lux::domain::WorldObjectId{uuids::uuid{object_bytes}}},
        true,
        {}};
    const ScriptBindingDescription update{
        1U,
        HookScriptTarget{system.instanceId(), UpdateHook}};
    const ScriptBindingDescription collision{
        2U,
        EventScriptTarget{system.instanceId(), CollisionEvent}};
    assert(addBinding(*simulation, asset, mount, update) ==
        EScriptAuthoringError::SUCCESS);
    assert(addBinding(*simulation, asset, mount, collision) ==
        EScriptAuthoringError::SUCCESS);
    assert(addBinding(*simulation, asset, mount, update) ==
        EScriptAuthoringError::DUPLICATE_BINDING);
    assert(moveBinding(mount, 1U, 0U) == EScriptAuthoringError::SUCCESS);
    assert(mount.bindings[0] == collision);
    assert(eraseBinding(mount, 1U) == EScriptAuthoringError::SUCCESS);

    mount.bindings.push_back({99U, hooks[0]});
    const auto diagnostics = diagnoseBindings(*simulation, asset, mount);
    assert(diagnostics.size() == 1U);
    assert(diagnostics[0].error == EScriptAuthoringError::MISSING_SYMBOL);

    ScriptMountDescription global_mount{
        ScriptMountId{2U},
        lux::asset::AssetId{asset_bytes},
        SimulationScriptMount{},
        true,
        {}};
    assert(addBinding(
        *simulation,
        asset,
        global_mount,
        collision) == EScriptAuthoringError::SCOPE_MISMATCH);
    return 0;
}
