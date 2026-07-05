# `gameplay/2d` — the 2D dimension kit (`lux::gameplay::d2`)

A peer of `gameplay/3d` (`lux::gameplay::d3`): a self-contained kit of ECS
components + systems + renderable bridges for 2D games, built on the same
domain-neutral `World` / `ISystem` / `RenderableSystem` mechanisms. `d2` and `d3`
are siblings — neither depends on the other, and `World` hardcodes no systems.

Full design: `.internal/lux-engine-2d-pack-design.md`.

## Layout (mirrors `3d/`)

```
2d/
  Scene2D.hpp        install entry (free functions, not a class)
  D2ScenePlan.hpp    capability collection consumed by install()
  world/components/  2D components (Transform2D, Sprite, Collider, PixelField, …)
  world/systems/     2D systems (Transform2D, Camera2D, Simulation2D, …)
  traits/            custom render bridges' trait glue
src/Scene2D.cpp      + one .cpp per system
```

## Two rules that shape this kit

1. **Plan-driven install (not per-`installXxx`).** The neutral `World` is
   append-only — `addSystem` appends in registration order with no
   remove/replace/reorder. So a `D2ScenePlan` COLLECTS capabilities and
   `d2::install()` turns it into ONE deterministic system order in a single pass.
   This is required because 2D physics must run *before* `Transform2DSystem`
   (unlike d3's Transform→Camera→Animation), and because every fixed-step
   capability must be demoted into one unified `Simulation2DSystem` phase rather
   than each owning its own accumulator. (design §2.5)

2. **Payment symmetry.** A traditional 2D plan (`enableCore().enableSpriteAnimation()`)
   installs no pixel-simulation system and registers no pixel bridge, so it pays
   zero runtime cost for Noita-style features. Conversely a pixel plan can omit
   sprites. Capabilities are opt-in layers; the base (`Core` = Transform2D +
   Camera2D + ordered layers → swapchain) is all any 2D scene needs. (design §1.3)

## Renderable bridges are CUSTOM

2D does NOT reuse the generic `INSTANCE`/`POOL` bridges (INSTANCE is hardcoded to
mesh/material assets). Every 2D renderable uses a custom `IRenderableBridge`
registered via `RenderableSystem::addBridge`, handing draw batches to the render
side's `Canvas2DFeature`. Each custom bridge's `reap` MUST use
`inComponentView<C>(reg, e, Require{}, Exclude{})` (the Gate -1 / G-01 contract).

## Scene lifecycle contract (D-04)

The scene container (the app's / editor's 2D equivalent of `EditorScene`) owns the
services and tears them down in a fixed order — mirroring the Gate -1 two-phase
teardown, so **no object relies on its destructor to send GPU/field commands**:

```
teardown:
  1. stop gameplay writes        (no more command-buffer / field mutation)
  2. bridge shutdown             (RenderableSystem two-phase drain — beginShutdown →
                                  drain → flushShutdownCleanup, reclaims GPU caches)
  3. system shutdown             (drop the World's systems)
  4. runtime destroy             (PixelFieldRuntime: destroy fields, free chunks)
  5. World destroy               (entities/components last)
```

Two invariants:

- **Non-owning refs outlive their users.** `PixelFieldRuntime` (and any injected
  service) is passed by non-owning pointer/reference into systems + bridges; it MUST
  outlive them (destroyed at step 4, after the systems at step 3). Same rule as
  `RenderableSystem` taking a `RenderSession&`.
- **Half-initialised bring-up rolls back in reverse.** If constructing service *k*
  fails, services *k-1 … 1* are destroyed in reverse construction order (RAII / a
  reverse-ordered rollback), leaving no partially-wired scene. See
  `construction_failure_rollback_test`.

`PixelFieldRuntime` itself is forward-declared for now (the install API only needs a
pointer); its create/destroy/read/command interface + handle lifecycle are implemented
by a later pixel-field task.
