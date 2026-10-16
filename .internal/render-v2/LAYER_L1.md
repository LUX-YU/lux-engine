# L1 — registration and resource ownership checkpoint

Parent L0: 5623b5c1b4f7cbaa254913dc4006e38383dd91ed. This is an implementation gate, not independent final V.

Texture declarations and retained-byte accounting moved from features to resource. Native upload consumes TextureWrite/TextureRegionWrite values, which the corresponding Commands alias without a payload copy. StorageRegistration/nativeStorageBinding moved out of Factory.hpp.

CommandHandler is generated from an exact noexcept State/Request/CommandContext signature. Its erased constructor is private. CommandRegistration adds only cold binding/physical-completion qualification; Protocol contains neither DEVICE_SESSION policy nor gpu_completion. Native registration binds the existing DeviceSession once before capability publication. Builtins, real Texture, external TestBuffer, batch, CPU async, and DLL-tail providers use typed generation. Remaining raw invocation cases are historical fault/boundary fixtures using a test-only friend, not a public author API. Ordinary GPU_READY handlers remain rejected, and ordinary complete cannot claim GPU success.

makeStorageFactory binds Declaration/Config/Ops/Native/Create together. Its raw constructor is private; malformed metadata and metadata-cost fixtures use an explicitly test-only friend. Config and Ops compare full lux-cxx TypeToken identity plus size/alignment before casts. Tests reject same-size wrong Config and Ops. StorageRegistration's typed configuration qualification is checked before startup. Factory pin/config destruction order is unchanged.

DeviceSession no longer installs Texture. Server installs exactly its explicit cold registrations and rejects duplicates; direct Native fixtures install what they use. A real Vulkan Session with no Texture registration and an explicitly installed alternative implementation prove that no hidden default shadows the selected factory.

render_protocol_async is a real interface component for AsyncCommand/stdexec. Synchronous Protocol no longer links/finds stdexec. Resource CpuSchedule still carries stdexec until separate E1; this checkpoint does not claim execution-layer relocation.

## Local gate evidence

- RelWithDebInfo: full all -j 4 -- -k 0 succeeded; second all reported ninja: no work to do.
- CTest -V --output-on-failure -j 1 -R .: 100/100, all original 98 names retained; added render.protocol.binding and render.native.explicit_factory.
- Fresh relocated Protocol and Client consumers passed with stdexec lookup explicitly disabled; Client also disables Vulkan.
- Existing real GPU/Validation/SyncValidation, recovery, two-client, async-tail, LastUse, and TestBuffer extension tests passed. Existing EOS duplicate-layer warnings remain in complete stdout.
- 719 Legacy working blobs and baseline blobs, old reports/checkers, IPC source, original user files and dependency commits passed protection checks.
- A prior local run was 98/99: the manual Offscreen registration omitted new Config qualification and was rejected at startup. The fixture now supplies its actual type; registration rejects mismatch before run. Failure log retained.
- Debug/ASan and independent clone verification remain L4 obligations. No performance qualification upgraded; IPC/alias PARTIAL and Root product BLOCKED remain historical facts.

Commands: E:/RenderV2Rebuild/Layer/run.ps1 -Action build (twice), then -Action test. Exact source/build/dependency paths are in every log. layer-l1-evidence.json hashes complete stdout and protection evidence. L2 may now proceed; final publication requires L4.
