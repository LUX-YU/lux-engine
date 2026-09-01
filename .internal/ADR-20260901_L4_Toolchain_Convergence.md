# ADR: L4 Toolchain and Material Compiler Convergence

Status: Accepted

Date: 2026-09-01

Baseline: `bb2b12b3f425db16eca6a96828a101fd3ae9bad5`

## Decision

The canonical upper-layer ontology is:

```text
L4 Toolchain
L5 Editor
L6 Host / Product
```

Authoring is retired as an architecture layer. MaterialGraph becomes a reusable Toolchain/Editor source model in
`modules/function/material`; Material compilation becomes the L4 transformation
`MaterialGraph -> MaterialDescription`; the Material cooker delegates compilation and only creates the typed Asset.
The remaining standalone Script Authoring package has no production consumer and is deleted with the retired
Authoring root. Its last retained implementation is recoverable from the baseline commit above.

FlowForge remains the reference source/compiler/artifact pipeline. Editor node_graph remains an L5 editing mechanism
and does not become a compiler, source model or IR. No common Graph or Compiler framework is introduced.

## Consequences

- Runtime and PLAYER never link MaterialGraph, Material compiler/cooker, shaderc, FlowForge compiler or GraphKit.
- EDITOR configures MaterialGraph but does not configure the Material compiler until a real editor consumer exists.
- TOOLCHAIN and packed-render host builds configure the Material compiler and cooker.
- MaterialIR, ShaderIR, lowering and backend contracts remain compiler-private.
- No old include, namespace, target, package, alias or forwarding header is retained.
