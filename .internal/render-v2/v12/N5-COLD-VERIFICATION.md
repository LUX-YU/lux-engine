# N5 cold contracts — independent checkpoint

Implementation `1ae59973956ecdd9905ee17903660b800efc6b1d`, parent `752ad5dcf379d8165c72291222af23833e405195`. Remote tracking ref observed `78bcd220815f845466d85d31f994901e4db224ba`; no push.
Exact detached clean clone `E:\RenderV2Rebuild\V12\n5-cold-verification`, `--no-hardlinks`, no alternates. Tracked snapshot passed before
configuration. Verification source remained unchanged. V adds only this report and the evidence index.

Independent Windows/MSVC ASan `/fsanitize=address /Zi`, C++20 `/GR-`,
`ASAN_OPTIONS=alloc_dealloc_mismatch=1`: **106/106**, 166.13s. Fresh configure, full
`cmake --build <build> --target all -j 4 -- -k 0`, repeated build no work, then
`ctest --test-dir <build> --output-on-failure -j 1 -R .`. All 102 baseline names retained.
Public headers and installed/relocated/external consumers are included in CTest. Real Vulkan
Validation/SyncValidation enabled, errors zero; ASan errors zero. Actual overlay warnings retained.

Implementation Release had 106/106 before the final attach-key and dead-Feature callback checks;
affected final tests passed afterward. This checkpoint does not claim a new independent Debug or
Release full matrix. The final N7 gate still requires all three configurations.

Typed shared ConfigValue equality and cross-DLL code-pin lifetime pass; non-comparable distinct
values reject rather than compare padding. Texture/Buffer attachment values use typed ConfigValue,
not command-local codecs. Unknown, missing, wrong-type and conflicting arguments fail at cold
boundaries. Dead resource/Shader contracts do not enter Native lowering; exported/live equivalents
still fail. Zero-live-Pass contributions need no Program callback. Original pixels, LastUse,
Pipeline/Bind/Scope counts and shutdown behavior remain covered.

Legacy 719 blobs, user worktrees, dependencies, historical evidence and whitelist checks pass.
Three installed include prefixes were backed up and synchronized from exact I; Android headers do
not imply an Android build. Traceability checker verifies allocation/completeness only, not PASS.

**N5.1/N5.2 tested cold subset PASS; whole N5 IN_PROGRESS.** Unique layout relocation, full shader
ABI, parameter plans, execution primitives and rebind/readback work remain. N6/N7 not completed.
Historical IPC/Native/Alias performance remains PARTIAL, full Root TOOLCHAIN remains historical
BLOCKED. Upper Engine/Editor, Linux/Android and TSan NOT_RUN; no new performance claim.

Portable raw logs, failed attempts, SDK backups and commands: `E:\RenderV2Rebuild\V12\archive\1ae59973956ecdd9905ee17903660b800efc6b1d\n5-cold-evidence.zip`.
SHA-256 `dba1337fa5c4b25c5f480ab4abd0db7bb52f27c67c779587cd6765dfc113751f`. Per-file hashes are in the adjacent evidence index.
