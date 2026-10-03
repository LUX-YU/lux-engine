#include "AssetSceneFixture.hpp"
int main(int argc, char** argv)
{
    assert(argc == 3);
    Fixture fixture{argv[1], argv[2]};
    auto timer = fixture.execution.timer();
    const std::array providers{
        lux::scene::makeSceneCapabilityProvider<lux::scene::ScriptRuntimeHost>(
            "script-runtime", "lux.script.runtime.host", *fixture.host),
        lux::scene::makeSceneCapabilityProvider<process::TimerClient>("timer", "lux.process.timer", timer)
    };
    auto lease = take(fixture.runtime->builder()
        .setDescription(std::shared_ptr<const lux::scene::SceneDescription>(fixture.package.scene, &fixture.package.scene->data()))
        .setWorld(std::shared_ptr<const world::WorldDescription>(fixture.package.world, &fixture.package.world->data()))
        .setSimulation(std::shared_ptr<const SimulationDescription>(fixture.package.simulation, &fixture.package.simulation->data()))
        .setRegistrations(fixture.schemas, *fixture.systems, fixture.registrations).setProviders(providers)
        .setClock(take(lux::scene::FixedStepClock::create(std::chrono::milliseconds{1}))).build());
    const auto id = lease.id();
    fixture.frame(); // Starts the original Hook coroutine and its first real asset read.
    assert(fixture.value(id) == 7);
    assert(fixture.runtime->pauseSimulation(id));
    for (int frame{}; frame != 6; ++frame)
    {
        fixture.frame(); // Completed native work can settle while script evolution is paused.
        assert(fixture.value(id) == 7);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    assert(fixture.runtime->resumeSimulation(id));
    for (int frame{}; frame != 30; ++frame)
    {
        fixture.frame();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    assert(fixture.value(id) == 9);
    fixture.verifyFinished(id);
    auto retired = take(fixture.runtime->retireInstance(id));
    lease = {};
    for (int frame{}; frame != 4; ++frame) fixture.frame();
    assert(!fixture.runtime->borrowInstance(id));
    assert(fixture.backend.stats().prepared_ability_slots == 0);
    assert(fixture.files.join());
    std::puts("EC2 actual SceneRuntime: packaged Lua/VFS, native assets, pause, command barrier, retirement PASS");
}
