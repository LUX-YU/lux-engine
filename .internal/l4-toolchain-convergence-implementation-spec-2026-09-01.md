# L4 Toolchain / Material Compiler Convergence Implementation Spec

Status: Implementation baseline

Date: 2026-09-01

Baseline: `bb2b12b3f425db16eca6a96828a101fd3ae9bad5`

## Frozen topology

```text
modules/function/material
    MaterialGraph source model

engine/toolchain/material
    MaterialGraph -> MaterialDescription compiler
    MaterialGraph/ImportedMaterialDescription -> MaterialAsset cooker

engine/editor/node_graph
    domain-independent L5 editing UI
```

Public source namespace is `lux::material`. Public source includes live below
`lux/engine/material/graph/`. Compiler and cooker entry headers are `lux/engine/material/Compiler.hpp` and
`lux/engine/material/Cooker.hpp`.

The source target is `material_graph` (`lux::engine::function::material_graph`). The L4 targets are
`toolchain_material_compiler` and `toolchain_material_cooker`, installed by `lux-engine-toolchain-material`.

## Contract split

- Source-only value/attribute/input types move out of Resource Description.
- Runtime `EAlphaMode` and `ELightingTechnique` remain in Resource Description.
- `EMaterialPass`, MaterialIR, ShaderIR and backend contracts are private to the compiler.
- ShaderIR owns its value enum and does not depend on a Material graph contract.
- `compileMaterial(const MaterialGraph&)` is the only graph-to-description path.
- The cooker invokes that facade exactly once and performs only Asset-level construction and failure mapping.

## Holds

No Editor material panel, generic Graph/Compiler/IR framework, runtime compiler, Asset residency, Product streaming,
compiler registry, manager, context, services or compatibility layer is authorized by this work.
