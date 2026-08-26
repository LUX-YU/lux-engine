# L1 World

`lux::engine::world` owns the canonical descriptive World contract:

```text
UUID-addressed objects
typed versioned opaque data
domain-neutral partition build products
query-free partitioner/workspace interfaces
```

It intentionally has no dependency on ECS, EnTT, Simulation, Scene, Asset,
TaskGraph, or a spatial/math domain. Runtime state and behavior remain outside
this package.
