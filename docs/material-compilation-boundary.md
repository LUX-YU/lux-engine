# Material compilation contracts

`material_graph` owns author data, node contracts, validation callbacks and source codecs. Its public
headers contain neither Slang objects nor a shader expression representation. The separate
`material_compiler` implements purpose-aware direct Slang emission and consumes the shared
`shader_compiler`. See the [Material module](../modules/function/material/README.md) for the complete API.

`deriveMaterial(ShaderCompiler&, MaterialGraph const&, options)` returns owned target programs,
reflected layouts, diagnostics and runtime defaults. Temporary traversal indexes and emitted text
belong to that call. No graph, node code, Slang object or borrowed source view escapes in the result.
Parameter default changes and texture AssetIds do not enter the code key; emitted source, embedded
common code, target/profile, compiler version and material ABI do. The hash is an index, not proof of
program equality; Render compares actual program/layout values.

`ShaderCompiler` is a serial owner of its GlobalSession. Matching requests within one synchronous
immutable dependency batch share a target Session. Sessions retire at batch end, after results have
been copied, so subsequent source/dependency edits cannot reuse stale imported modules. Use separate
instances for parallel CPU scheduler work. Diagnostics, including successful warnings, and actual
parameter use reports are separate from the declared runtime layout.

Ordinary heap exhaustion remains fatal. Semantic failures retain structured codes, NodeId/Pin
locations and owned messages. A failed candidate never replaces a valid material.

The installed `material-domain` consumer checks author source roundtrips, graph validation, direct
emission, registration and a real versioned plugin DLL, as well as standalone public headers. It
links the module compiler. The former Engine toolchain compiler, its Backend/ShaderEmitter and
`engine/toolchain/material/test/domain_boundary.cpp` remain out of scope and require migration to
this API; they cannot be built against removed module expression headers through compatibility shims.
