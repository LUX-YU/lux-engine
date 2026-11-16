# Shared target shader compiler

`shader_compiler` compiles Slang source to an owned `ShaderProgramArtifact`, using
reflection from the same target session. It does not borrow reflection objects
past compilation or reuse SPIR-V offsets for DXIL. Supported descriptors cover
constant/structured/byte-address buffers, textures and samplers; unsupported
layouts, register collisions, scalar widths and stages return a diagnostic.
It is a cold compiler boundary, not a Render recording dependency.

`bootstrap/shader-tools.json` fixes Slang 2025.24 and DXC 1.8.2505.1 archives and
checksums. The Windows x64 packages are shared outside the source tree in
`LUX_SHADER_TOOL_CACHE`. DXC is loaded from the deployed compiler directory;
a missing deployment is an error. No Vulkan SDK or PATH compiler fallback is used.
Runtime libraries and upstream licenses are installed with `lux_engine_resource`.

```text
lux_shader_compile input.slang main fragment spirv output.spv
lux_shader_compile input.slang main fragment dxil output.dxil
```

Each invocation writes binary code and a `.meta` reflection file. These are target
artifacts, not a proof of device execution. The existing `shader_metadata` value
codec is reused and remains independently linkable without Slang.
