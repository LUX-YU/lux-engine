# ADR: Finite Pixel Migration Classification

- Status: Accepted for implementation wave 1
- Date: 2026-08-30
- Base: `5f9f90a9296ba18666c7eb09c895b57a8c8977ca`

## Canonical ownership

Finite mutable cellular simulation belongs to **L1 Simulation**. It is not an ECS architecture layer, World storage, Scene composition, Render functionality, or Process orchestration.

The first production slice owns exactly one finite sparse field per `PixelFieldRuntime`. Large cell/chunk state stays outside EnTT component storage. A future concrete Pixel System may own field runtimes and associate them with entities, but this wave does not invent that ownership before a real ECS consumer exists.

## Legacy salvage

Salvage behavior/algorithms from `legacy/ecs/pixel`:

- sparse 256x256 chunk storage;
- active-tile stepping rather than whole-logical-world scans;
- stable local material identifiers;
- deterministic update order and state hashing;
- missing resident chunks acting as simulation boundaries;
- steady-state stepping without general-heap allocation.

Do **not** restore the legacy outer shape:

- multi-field SlotMap/handle manager semantics;
- `parallelism` or a Pixel-owned worker pool;
- persistence snapshots/deltas;
- render dirty export/ack accounting;
- streaming/residency ownership;
- physics adapters;
- old ECS/System interfaces;
- content UUID compatibility fields;
- Scene/Runtime bridges.

## Wave-1 public type budget

Only the following Pixel-domain public vocabulary is approved:

- `PixelMaterialId`;
- `EPixelMaterialPhase`;
- `PixelMaterialDefinition`;
- `PixelFieldConfiguration`;
- `EPixelFieldError`;
- `PixelFieldRuntime`.

No Manager, Context, Registry, Bridge, Adapter, Service, Loader, Storage, Streaming, Presentation, or ThreadPool type is approved by this ADR.

## Semantics

- Coordinates are field-local integer cell coordinates. World placement belongs to Transform/Scene consumers, not the cellular kernel.
- The field is bounded for this first probe. Infinite streaming is a later World/Scene/System integration problem.
- Chunks are created only by explicit mutation. Simulation never allocates a missing neighboring chunk; a missing resident chunk is a solid simulation boundary.
- A logical tick processes only currently active resident tiles.
- Powder and liquid movement is deterministic. A moved destination cell is not advanced twice in the same logical tick.
- The first implementation is serial. Parallel recovery waits for the existing shared CPU execution resource and must preserve the serial state hash.

## Explicitly held

- Pixel persistence / World chunk schema;
- infinite 2D Pixel streaming;
- dirty-region Presentation export;
- Pixel/Physics2D coupling;
- generic streaming markers;
- generic asset demand/residency;
- Pixel-specific threads or schedulers.

Compilation compatibility with legacy is not a migration goal. New tests must exercise the native API and retain only the useful behavioral assertions.