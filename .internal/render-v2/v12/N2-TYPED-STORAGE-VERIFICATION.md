# Independent Typed Storage/Handler installation checkpoint V

I `77911a6b8ba92210b44c0d2e7d2eff752df1171b`, parent `6f4aee882f7239f8b4f170adb9ff79e233b1050b`. Detached clean no-hardlinks clone E:\RenderV2Rebuild\V12\n2-typed-storage-verification, no alternates;
fresh build E:/RenderV2Rebuild/V12/n2-typed-storage-asan. No source repairs in verification. Script verify-n2-typed-storage.ps1
runs tracked snapshot, configure, all -j 4 -- -k 0, second no-work and complete CTest. MSVC C++20 /GR-,
/fsanitize=address /Zi with alloc_dealloc_mismatch=1. All 106/106 passed, 285.95 s. Validation,
SyncValidation and ASan errors zero; actual warnings retained. Header/install/relocation consumers included.
Implementation RelWithDebInfo: 106/106, 214.91 s, plus focused 9/9. Old erased Storage consumers and a heavy Native header leak were corrected in implementation;
failed attempts preserved. Public-header dependency guards were not weakened. Route layout observed call16/8,
entry24/8, metadata160, request256 bytes. No performance improvement/equivalence inferred.

Tests prove one typed provider supplies matching Factory/Native/Config/Ops and real member Handlers.
Same type/key/config is idempotent; another key adds support without stealing active; incompatible
config/implementation leaves existing Commands intact. Texture and external Buffer use the same
installation entry and unique Handler Owner; the graph-only Buffer needs no dummy Handler. Original
GPU upload/readback/LastUse behavior is retained. Source Code Pin remains alive after Server until
cached canonical operations and returned directory values finish, then releases once. No production
IPC, Queue, synchronization or Transfer algorithm changes. Old StorageRegistration and
nativeStorageBinding public interfaces are deleted, not forwarded.

Legacy719, all three protected user files, dependencies and history pass the supplemental checker.
This prerequisite PASS does not close joint N2/N3. Current uncommitted Feature provider/old CommandRegistration deletion
migration is excluded. N4-N7 not started; performance PARTIAL; root product historical BLOCKED;
upper Engine/Editor, Linux/Android/TSan NOT_RUN. Final N7 matrix/three-prefix sync still required. No push.

Immutable archive: E:\RenderV2Rebuild\V12\archive\77911a6b8ba92210b44c0d2e7d2eff752df1171b\typed-storage
Portable ZIP: E:\RenderV2Rebuild\V12\archive\77911a6b8ba92210b44c0d2e7d2eff752df1171b\typed-storage-evidence.zip
ZIP SHA-256 `97933a3fcf5560371ae0119ff2a558e63bfd48751b7787054ae66275d006e643`
Index N2-TYPED-STORAGE-evidence-index.json SHA-256 `e0ea6d491f5f6ef5c7cb400e941a5a9eadf57c5cc86e9f5b44e718eefb0e80f1`.
Contains raw logs, CSV, scripts, command/exit records, failed attempts and protection evidence.
