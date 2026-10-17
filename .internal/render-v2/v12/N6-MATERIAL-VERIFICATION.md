# N6 Material — independent checkpoint

Exact I `d0c82ccd0f7b0627d2d37be3a4b4d2d82793c9f6`, parent `1129fbb0fe20b136059febe94623019aaf82fbd7`. Remote observed `78bcd220815f845466d85d31f994901e4db224ba`, no push.
Detached --no-hardlinks clean clone `E:\RenderV2Rebuild\V12\n6-material-verification`, no alternates. Tracked snapshot before fresh configuration;
verification source unchanged. V only adds this report/index.

Windows MSVC ASan /fsanitize=address /Zi /GR-, ASAN_OPTIONS=alloc_dealloc_mismatch=1:
**136/136**, 189.85s. Full all-j4--k0, second no-work and serial CTest. All102 original names, all116 prior
Render/Platform tests plus17 existing material tool dependency tests and3 new Material tests retained.
Fresh/relocated SDK executes actual Material upload/query/release with copied/hash-checked Cooked asset;
public headers, pure Client/no Vulkan and pure sync/no stdexec consumers pass. Validation/Sync Error0,
ASan Error0; real duplicate EOS overlay warning retained. Exact-I fresh Debug/Release not run at this
checkpoint; implementation Release136/136134.30s and final stronger4/414.18s recorded separately.

Real existing MaterialGraph/lowering/emitter generates Unlit and textured SPIR-V. Native validates full
416-byte Cooked record ABI, performs actual shared Transfer and three Queue-mode GPU outputs64/191/96,
one nonzero Pipeline compilation with no parameter/Texture-content recompilation, two Contexts, failed
Submit preserving last-good, stale/recovery/CodePin lifetime, explicit version-pinned Textures and real
logical Owner deletion before GPU completion observation. LastUse/retirement remains the sole safety
proof. Recording window new0 is limited to that test window, not a complete-request allocation claim.

The MaterialStorage component is qualified in this tested scope. N6.2 overall still awaits integrated
BasicMesh consumer; N6.3–5/N7 remain unfinished. Structure changes use cold new program/Effect candidates;
bound incompatible changes are refused without modifying the active Graph. No MaterialGraph replacement,
advanced material/alpha blending, automatic follow-current Material texture contents or GPU performance
improvement is claimed. TexturePublished versions are an explicit content contract; direct TextureBinding
still follows current as before. Original Shader author tools remain build-time dependencies only.

Protection checker passes719 frozen blobs and3 protected user files in source and independent clone;
old reports/checkers/IPC algorithms unchanged. SDK sync70 headers13obsolete removals249backup/hash records.
Historical performance/IPC PARTIAL, Root TOOLCHAIN historical BLOCKED and upper Engine/Editor NOT_RUN are
unchanged. Linux/Android/TSan NOT_RUN, GPU timestamp NOT_MEASURED. Final N7 requires comprehensive R/D/ASan.

Commands and return codes: archive run.ps1; Source=E:\RenderV2Rebuild\V12\n6-material-verification, Build=E:/RenderV2Rebuild/V12/n6-material-build/a,
Evidence=.../evidence/N6-material/independent/a; snapshot/configure/build/build/test. Complete raw logs,
failed implementation attempts, generated samples, SDK backups, source diff and dependency/protection
snapshots are indexed in N6-MATERIAL-evidence-index.json.
Portable `E:\RenderV2Rebuild\V12\archive\d0c82ccd0f7b0627d2d37be3a4b4d2d82793c9f6\n6-material-evidence.zip` SHA-256 `afa01ff5479644a1a902d5286f565b9640301ab63f9b15e3c468ce6d1d838e91`. No V source fixes. V self SHA is reported after commit.
