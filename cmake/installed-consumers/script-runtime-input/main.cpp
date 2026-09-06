#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <array>
#include <cassert>

int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    SimulationDescriptionBuilder builder;
    auto simulation = std::move(builder).build();
    assert(simulation);
    ecs::Registry registry;
    SimulationClock clock;
    std::array<std::uint8_t, 16U> bytes{};
    bytes[0] = 1U;
    const auto entity = registry.create();
    const std::array inputs{ScriptRuntimeMount{{1U}, lux::asset::AssetId{bytes}, EntityScriptScope{entity}, {}}};
    const auto capacity = planScriptRuntimeCapacity(inputs);
    assert(capacity);
    const ScriptArtifactResolver missing{nullptr,
        [](void*, const lux::asset::AssetId&, ResolvedScriptArtifact&) noexcept { return false; }};
    auto runtime = ScriptSystem::create(*simulation, *capacity, inputs, registry, clock,
        {1U, 1U, 1U, 1U, 1U, 1U, 64U, 1U, 1U, 1U, 1U, 1U}, missing, {}, {}, {}, {});
    assert(runtime);
    const auto status = runtime->queryMountStatus({1U});
    assert(status && *status);
    assert((**status).submission_state == EScriptMountSubmissionState::ACCEPTED);
    assert(std::get<EntityScriptScope>((**status).submitted_scope).self == entity);
    const auto prepared = runtime->prepare();
    assert(!prepared && prepared.error() == EScriptSystemError::ASSET_NOT_RESIDENT);
    assert(runtime->shutdown());
}
