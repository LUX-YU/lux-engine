# L4 Toolchain Convergence

Status: **Closed**

Baseline: `bb2b12b3f425db16eca6a96828a101fd3ae9bad5`

Qualified implementation: `fe46e7ecf47c38d4d57131101fdcfc37e3327115`

The previous qualification remains valid historical evidence. A subsequent source-level review found four P1
closure items; revision `fe46e7ec` closes and qualifies all four: malformed MaterialGraph input fails before lowering,
the installed Material compiler is relocatable, shared Shader support has a canonical Toolchain owner, and GraphKit
compound transactions are atomic. L1-L3 remained closed throughout the hardening.

Date: 2026-09-01

## Result

The active architecture ontology is now:

```text
L0-L3 Runtime Foundation
L4 Toolchain
L5 Editor
L6 Host / Product
```

`Authoring` is no longer an architecture layer. The Material source model is a Function-owned Toolchain product,
while graph compilation and cooking are L4 Toolchain responsibilities. The unused Script Authoring surface and the
entire active `engine/authoring` root were removed without a forwarding header, target alias or compatibility
namespace.

## Canonical Material topology

```text
modules/function/material
  material_graph
  lux::material::MaterialGraph
  lux::material::EValueType

engine/toolchain/material
  toolchain_material_compiler
    compileMaterial(MaterialGraph) -> MaterialDescription

  toolchain_material_cooker
    cookMaterial(...)
    cookImportedMaterial(...)
```

`MaterialGraph` is the single editable source SSOT. `compileMaterial()` is the single graph-to-description pipeline.
The cooker delegates to that public compiler seam exactly once and then creates the typed `MaterialAsset`.
`ImportedMaterialDescription` and the Model cooker converge through the same path; there is no second lowering,
emission or shader compilation orchestration.

The compiler owns private `MaterialIR`, lowering, shader IR, shader emission and backend contracts. Shaderc,
SPIR-V reflection and Render shader include paths are private implementation dependencies. None of those types or
dependencies appear in the installed compiler interface.

## Profile closure

- Default Developer and PLAYER do not configure MaterialGraph, the Material compiler or the Material cooker.
- EDITOR configures `material_graph` and `node_graph_editor`, but not the compiler/cooker or shaderc closure.
- TOOLCHAIN configures MaterialGraph, compiler, cooker, Model and packer products.
- Packed Render host builds use the same active compiler/cooker and asset packer.
- FlowForge and Editor node-graph ownership were not changed.

## Preserved contracts

- `rdesc::MaterialDescription`, `EAlphaMode` and `ELightingTechnique` remain Runtime description contracts.
- MaterialAsset v4 payload, generated GBuffer/Forward SPIR-V and ShaderInfo remain byte-identical for the same source
  identity and source-path metadata.
- Runtime and PLAYER closures contain no L4 Material implementation, shaderc, Editor or FlowForge compiler.
- L1-L3 ownership, SceneMeta, Render extraction, Asset residency and Product streaming were not changed.

## Pre-L5 closure

- Built-in Material nodes are final and external code cannot forge a node kind/payload pair.
- Every mutable Material source enum, payload, pin, reference and finite-value contract is validated before lowering.
- The Material compiler resolves Shader includes from immutable build-time embedded bytes and contains no original
  source/build path dependency.
- Shared SPIR-V reflection object code and the LGLSL emitter belong to `engine/toolchain/shader`; no package consumes
  a sibling `pinclude`.
- GraphKit failed compound edits, undo and redo restore document, history and revision through an inverse journal.
- `node_graph_editor` is classified as EDITOR / EDITOR / FOUNDATION.

## Held work

Asset residency, Product streaming, Script capability injection, GPU CI and generic compiler/graph infrastructure
remain held. No Manager, Context, Services, Registry, generic IR framework, compatibility alias or parallel Material
compiler path was introduced.
