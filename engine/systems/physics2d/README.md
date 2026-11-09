# Physics2D construction

`Physics2DSystem::create()` validates configuration before acquiring a Box2D world. A successful result
owns a complete world and its prepared body storage; there is no subsequent `prepare()` call.
Invalid configuration returns `INVALID_CONFIGURATION`. Exhaustion of Box2D's finite world table returns
`CAPACITY_EXCEEDED`; ordinary heap exhaustion follows the engine's fatal OOM policy.

The registered Simulation installer transfers the complete owner through
`SimulationSystemInstaller::addSystem()`. Rejected admission leaves the candidate owner unchanged.
The System borrows its registry and Simulation time, which must outlive it, and releases the Box2D world
when its sole owner is destroyed. Construction does not alter the registry.

The native and installed-SDK lifecycle test uses the real Box2D backend to verify invalid configuration,
world-table exhaustion and recovery, complete-owner admission, body updates and queries, and final
capacity restoration. It does not substitute a mocked allocation failure for the backend limit.

## Script query binding

The Simulation installer only creates the Physics2D system and its task. It does not publish a script
capability into Simulation. Professional script assembly borrows the configured System instance and
uses `Physics2DSystem::scriptAbility()` to obtain the existing generated PhysicsQuery2D binding.
Include `lux/engine/function/script/ScriptAbility.hpp` when consuming that returned value.
The System and its module must outlive ScriptSystem's borrowed binding; Scene destroys ScriptSystem first.

A Scene script binder can capture the actual selected ID, without a hard-coded provider in Simulation:

```cpp
[physics_id](lux::simulation::Simulation& simulation, lux::scene::ScriptSimulationInputs& inputs) noexcept
{
    auto* physics = simulation.findSystem<lux::physics2d::Physics2DSystem>(physics_id);
    if (!physics)
    {
        return false;
    }
    inputs.capabilities.push_back(lux::simulation::script::publishScriptAbility(physics->scriptAbility()));
    return true;
}
```

Store the binder in the product's `ScriptRuntimeHost::simulation_bindings` span for cold Scene assembly.
This is an explicit professional composition dependency, not a dependency from Physics2D on Scene or Editor.
