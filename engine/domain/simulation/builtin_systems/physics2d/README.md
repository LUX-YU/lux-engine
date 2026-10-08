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
