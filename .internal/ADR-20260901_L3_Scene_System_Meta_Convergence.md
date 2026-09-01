# ADR: L3 SceneSystem and Immutable Scene Metadata Convergence

- Status: Accepted
- Date: 2026-09-01
- Scene foundation: `eb2740faf511e597bf38a96ffa6234b8f812a8da`
- Runtime/meta closure: `23ef26e11cdd8eacf2050c8d8b23a842de2283d0`

## Decision

`SceneDescription` is a canonical owning durable composition of World/Simulation AssetIds, SceneSystem instances,
portable configuration, external requirement bindings and explicit dependency edges. Scene wire version 2 is the only
active format; version 1 has no decoder and repository content is recooked.

SceneSystem is a narrow compiled installer contract. External providers are exact name/capability/Contract-TypeToken
records used only during cold construction. SceneSystem dependencies use the shared deterministic System ordering helper;
stable-point and Presentation hooks are immutable serial vectors, not a second TaskGraph.

`SceneMetaManager` is built once after ReflectionRegistry startup and is immutable thereafter. It owns Component schemas,
SimulationSystem registrations, SceneSystem registrations, default portable configuration and exact cross-reference
indexes. It provides metadata queries, not runtime service lookup or hot plugin mutation.

Component semantic/editor classification is part of the unique generated `ComponentSchema`: Transform/Parent are
foundation, WorldTransform is hidden runtime-derived state, and Mesh/Light are visible domain contracts.

## Evidence

- RelWithDebInfo full build passed; second build reported `ninja: no work to do`.
- VS Developer PowerShell CTest: 146/146 passed.
- Installed consumers passed: Scene foundation, Scene meta, Scene core and Scene asset.
- Scene v2 inner payload golden: 276 bytes,
  SHA-256 `990666a328e50f4f65592b822b8d09f8bead3823de41dafd55890af20bc599b0`.

## Held

Asset residency, SpatialMeta, plugin hot reload and a production RenderRuntime remain outside this decision. Static Render
feature metadata and the concrete Render SceneSystem are supplied by the following convergence waves.
