# N6 material asset dependency checkpoint

Parent bac7faf247cc7caec69ada48fb0d3bde4dfce8c4. Existing MaterialGraph/lowerMaterial and Engine
ShaderEmitter generate actual Unlit SPIR-V offline; no runtime Render target links the tool or material
compiler. Real graph/object/identity dependencies and all their tests are included. MaterialGraph's
implementation-only Description dependency was removed in favour of its consumed neutral headers.
No Script, Legacy or placeholder target substitutes. Three verbatim shader include extractions have
provenance hashes, and generated material-manifest.txt records actual inputs/tool version/output hashes.

Windows Release full all-j4--k0/no-work and CTest **133/133,130.60s**. This includes all116 previous tests
plus17 original dependency tests. Subsequent manifest-only CMake addition rebuilt all and second no-work.
First configure failed missing explicit stduuid dependency path; supplied real vcpkg stduuid/tomlplusplus.
First GLSL compilation required GL_GOOGLE_include_directive, supplied by offline glslang adapter after
version line; emitter algorithms unchanged. Failed logs preserved, no disabled tests. Protection719+3PASS.

Commands: E:/RenderV2Rebuild/V12/run-material.ps1 with Source=E:/RenderV2Rebuild/V12/source,
Build=E:/RenderV2Rebuild/V12/build/r, Evidence=E:/RenderV2Rebuild/V12/evidence/N6-material/tools,
Action configure/build/build/test. Exact dependency locations are in that external script.
This is an implementation checkpoint, not independent V or N6.2 PASS. MaterialStorage, Instance/Camera,
Features and integration remain pending; final N6/N7 verification must include these source dependencies.
Historical performance PARTIAL and product BLOCKED unchanged.
