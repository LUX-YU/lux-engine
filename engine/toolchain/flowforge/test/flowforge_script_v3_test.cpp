#include <lux/engine/toolchain/flowforge/ScriptCompiler.hpp>

#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationStepInfo.hpp>

#include <array>
#include <cassert>

namespace
{
    struct Contact final
    {
        std::uint32_t body{};
    };

    inline constexpr std::array kHooks{
        lux::simulation::makeSystemHookPoint<void(
            const lux::simulation::SimulationStepInfo&)>("after")};
    inline constexpr std::array kEvents{
        lux::simulation::makeSystemEvent<Contact>(
            "contact",
            kHooks[0],
            lux::simulation::ESystemEventTarget::ENTITY_TARGETED,
            "lux.event.Contact",
            1U
        )};
    inline constexpr lux::simulation::SystemDescription kPhysics{
        .canonical_name = "lux.physics",
        .version = 1U,
        .hooks = kHooks,
        .events = kEvents};
}

int main()
{
    lux::simulation::SimulationDescriptionBuilder builder;
    assert(builder.addSystem("physics", kPhysics));
    auto simulation = std::move(builder).build();
    assert(simulation);

    auto compiled = lux::flowforge::compileFlowForgeScript(
        *simulation,
        "gameplay.physics",
        lux::flowforge::FlowForgeStateLayout{0x1234U, 16U, {std::byte{1U}}}
    );
    assert(compiled);
    assert(compiled->description.schema_version == 3U);
    assert(compiled->description.exports.size() == 2U);
    assert(compiled->description.default_bindings.size() == 2U);
    assert(compiled->description.exports[0].args.size() == 1U);
    assert(
        compiled->description.exports[0].args[0].canonical_name ==
        "lux.simulation.SimulationStepInfo"
    );
    assert(compiled->description.exports[1].args.size() == 1U);
    assert(
        compiled->description.exports[1].args[0].canonical_name ==
        "lux.event.Contact"
    );
    assert(
        compiled->description.default_bindings[1].kind ==
        lux::rdesc::EScriptBindingKind::EVENT
    );
    assert(compiled->abi.symbols.size() == 2U);
    assert(compiled->abi.state.size == 16U);
    assert(lux::rdesc::validScriptDescription(compiled->description));
    return 0;
}
