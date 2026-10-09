# Material compilation contracts

`material_graph` provides the backend-neutral `lux::shadergen::ShaderIR` values and their sole
`computeFingerprint()` implementation. The public header is `lux/engine/material/ShaderIR.hpp`.
It also provides `MaterialCompileFailure.hpp`, containing the existing domain error codes and
owning diagnostic. Neither header needs a compiler, Engine context, Editor, or private Toolchain header.

`toolchain_material_compiler` consumes these contracts. Graph lowering, GLSL emission, shaderc,
SPIR-V reflection and the resulting MaterialDescription remain in the Toolchain component.
There is no duplicate fingerprint implementation or forwarding header at the old private path.

The fingerprint algorithm and its existing word-wise seed are unchanged. It describes shader
expressions, not the complete material: render state and shading model remain separate inputs.
Runtime parameter defaults and connected output defaults do not alter this expression fingerprint.
Unconnected output defaults do. Callers must not treat this hash as a persistent node-type identity.

Ordinary heap exhaustion remains fatal. The unused `EMaterialCompileError::ALLOCATION_FAILURE`
enumerator is removed; actual semantic failures, node identifiers and pin indices retain their meanings.

`material.shader_ir` exercises the actual exported fingerprint. With TOOLCHAIN tests enabled,
`material.domain_compiler` compiles four real graphs into eight SPIR-V results and checks rejected graphs.
The installed `material-domain` consumer defaults to only `material_graph`; enabling
`MATERIAL_DOMAIN_COMPILER` additionally verifies the real installed compiler.

This boundary is preparation for module-owned node registrations. It does not itself implement or
qualify MaterialNodeCatalog, FlowNodeCatalog, open payload compilation, or the MA08 graph-authority migration.
