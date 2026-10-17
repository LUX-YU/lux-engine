# Independent Storage installation transaction prerequisite V

I `8484de6467f3f21680a1c90d6c60640289e0c8d8`, parent `0717ced1cd07ecdb8f25b3d9f71dcc2ac5316eec`. Detached clean no-hardlinks clone E:\RenderV2Rebuild\V12\n2-storage-install-verification, no alternates;
fresh build E:/RenderV2Rebuild/V12/n2-storage-install-asan. No source repairs in verification. Script verify-n2-storage-install.ps1
runs tracked snapshot, configure, all -j 4 -- -k 0, second no-work and complete CTest. MSVC C++20 /GR-,
/fsanitize=address /Zi with alloc_dealloc_mismatch=1. All 106/106 passed, 160.35 s. Validation,
SyncValidation and ASan errors zero; actual warnings retained. Header/install/relocation consumers included.
Implementation RelWithDebInfo: 106/106, 210.87 s, plus focused 6/6. Missing direct test include in the first
build was corrected in implementation; failed attempt preserved. Route layout observed call16/8,
entry24/8, metadata160, request256 bytes. No performance improvement/equivalence inferred.

Tests prove Factory/config/capability admission precedes identity publication and instance creation;
late Owner-thread installation, actual ListStorages capabilities, explicit Client discovery, failed
registration not publishing Commands, returned names outliving Server and existing Native factory/
Texture/Buffer/GPU/LastUse/closure behavior. The deleted configureType/freeze contract is explicitly
superseded by one cold install transaction. Sole type Registry, DeviceSession and Handler ownership
remain. No production IPC, Queue, synchronization or Transfer algorithm changes.

Legacy719, all three protected user files, dependencies and history pass the supplemental checker.
This prerequisite PASS does not close joint N2/N3. Current uncommitted typed Provider/Handler/Factory
migration is excluded. N4-N7 not started; performance PARTIAL; root product historical BLOCKED;
upper Engine/Editor, Linux/Android/TSan NOT_RUN. Final N7 matrix/three-prefix sync still required. No push.

Immutable archive: E:\RenderV2Rebuild\V12\archive\8484de6467f3f21680a1c90d6c60640289e0c8d8\storage-install
Portable ZIP: E:\RenderV2Rebuild\V12\archive\8484de6467f3f21680a1c90d6c60640289e0c8d8\storage-install-evidence.zip
ZIP SHA-256 `f00dd37a8411603470b5be1055f4869776d51d96e0861f777b8391ea4d87318b`
Index N2-STORAGE-INSTALL-evidence-index.json SHA-256 `060d1f6ff85018f8d86c7fce829bac70dc5ca4ee4e3d26b2ac2b230a1a14ed34`.
Contains raw logs, CSV, scripts, command/exit records, failed attempts and protection evidence.
