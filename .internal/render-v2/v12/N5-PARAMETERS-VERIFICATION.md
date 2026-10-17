# N5 general parameters — independent checkpoint

Implementation `b8b657839e42140214b0e8be7f590150c5ab97f2`, parent `c37f70b690334a20626e9dd78fc79feb444ff6fc`. Remote tracking observed `78bcd220815f845466d85d31f994901e4db224ba`; no push.
Detached clean clone `E:\RenderV2Rebuild\V12\n5-parameters-verification`, no hardlinks/alternates; tracked snapshot before fresh configure.
V source unchanged. This commit adds only report/index; subsequent lglsl implementation WIP is excluded.

Windows MSVC ASan /fsanitize=address /Zi /GR-, alloc_dealloc_mismatch=1: **110/110**, 174.10s.
Full all -j4 -- -k0, second no-work, CTest -j1 -R .; all102 baseline and previous109 tests retained.
Installed/relocated SDK, independent public headers, pure Client and external Buffer consumers passed.
Real GPU Validation/SyncValidation Error0, ASan Error0; actual EOS duplicate warning retained.
Implementation Release110/110121.39s. No fresh independent Debug/Release for this precise I.

Shader parameter ABI now owns exact fields/stages; no implicit push16/Native color default. The same
LayoutPlan produces actual pipeline push ranges and prepared Storage/Context field copies. CompiledGraph
owns immutable defaults; Context owns mutable bytes; professional Storage use retains borrowed source.
True64B Vertex+Fragment parameter test combines Texture control, external Buffer gain, Context tint and
Pass bias, produces16/32 with independent Contexts. Buffer is read in both stages with actual stage barriers.
Nine invalid mapping/ABI cases reject, invalid updates retain old output; separate no-push Graph produces64.
Twenty update/record/submit iterations retain first-party new=0. No second owner/graph/registry introduced.

Context320B(previous296), PreparedBinding80, Binding32, RetirementEntry104;10k host ledger1,343,640B/61new.
Cold prepared parameter/copy vector cost is explicit, not a performance improvement claim. Historical PARTIAL
unchanged. Earlier fixture failures (missing explicit metadata/values and a renamed resource) preserved;
original pixel and Pipeline/Bind/Scope assertions retained. Native sampling still Fragment-only and Runtime
View.color remains for N6 Camera migration: this report does not qualify either broader capability.

Exact-I63 headers synchronized/backed up/hashed to three prefixes,13 deleted headers recorded. Android
sync is not a build. Legacy719,3 user files, history/dependencies/boundaries pass. Source/compiler/SPIRV
hashes and all attempts archived. **Parameter subset PASS; N5 IN_PROGRESS.** lglsl/graphics/compute,
N6/N7 and final matrix remain. Performance PARTIAL; product historical BLOCKED; upper Engine/Editor,
Linux/Android/TSan NOT_RUN. GPU timestamp for this checkpoint NOT_MEASURED.

Portable evidence `E:\RenderV2Rebuild\V12\archive\b8b657839e42140214b0e8be7f590150c5ab97f2\n5-parameters-evidence.zip`; SHA-256 `60e4b7f6c10c333643c404c85f254b8e38cf3c232a92b930b1c0dbbc86797922`. Per-file index adjacent.
