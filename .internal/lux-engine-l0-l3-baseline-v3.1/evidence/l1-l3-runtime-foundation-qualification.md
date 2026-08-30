# L1-L3 Runtime Foundation Qualification

- Date: 2026-08-30
- Qualification SHA: `7260b6bff7a05f6bb2ae05e05ab2765781c5e2ac`
- Build: RelWithDebInfo, Visual Studio 2022 Developer PowerShell 17.14.35
- Command: `target all -j 4 -k 0`
- First build: passed
- Second build: `ninja: no work to do.`
- CTest: 130/130 passed

## Implementation checkpoints

| SHA | Result |
|---|---|
| `ac58911b` | DOMAIN classifier, neutral WorldObject/Partition identities, Simulation-to-World edge removed |
| `45d41064` | World partition load workflow moved from Scene to Process |
| `ff222299` | typed Asset load Sender |
| `45f13d4a` | finite sparse Spatial3D index |
| `f91fa3b1` | finite sparse Spatial2D index |
| `7260b6bf` | hard gates and Held lifecycle/adoption reviews |

## Installed consumers

The following independently configured and built against the RelWithDebInfo install prefix:

```text
domain-world-identity
domain-partition
process-world
process-asset
scene-world-runtime
spatial3d-index
spatial2d-index
```

## Qualification evidence

- Target DAG positive/negative probes cover DOMAIN, WORLD, SIMULATION, PROCESS and SCENE directions.
- Source scans find no retired World-owned partition identity or Scene `WorldRuntime` loader aggregate.
- `process_world` retains exact range accounting, lazy extent traversal, cancellation and generation checks.
- `process_asset` covers Texture/Material/Model, synchronous/deferred completion, rejection, storage/decode failure,
  identity/type/limit validation, cancellation and retained `SharedBytes` ownership.
- Spatial2D/3D tests cover negative and `1e12` coordinates, exact cell boundaries, half-open bounds, sparse holes,
  overflow, all-or-none output capacity, canonical ordering and 1,000 allocation-free queries.
- World diagnostic benchmark at the qualification SHA:

```text
root_build:   1,000,000 partitions, 22,700 ns, 381 retained bytes
page_lookup:  1,000,000 lookups,   3,518,500 ns, checksum 1,000,000,000,000
layout_build:   100,000 objects,   29,627,700 ns
```

CPU Asset lifecycle, Spatial index wire/adoption and StreamingSystems remain `Needs more evidence`; no production
types were created for those gates. Android engine configure/build/CTest was not run; Debug, RelWithDebInfo and Android
install include trees were synchronized before dependent builds.

The user's pre-existing `.gitignore` and `WorldPartition.hpp` formatting changes remained outside all commits.
