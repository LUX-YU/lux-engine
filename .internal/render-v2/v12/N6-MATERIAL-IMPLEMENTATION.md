# N6.2 Material production checkpoint

Parent1129fbb0fe20b136059febe94623019aaf82fbd7 (existing lowering/emitter tool integration).
MaterialStorage is a real typed provider with Set/Query/Release Commands, professional GPU versions,
common Transfer, stable logical SlotKey, exact revision and immutable cooked program configuration.
Runtime/Server has no Material switch/table. One compiled Graph owns the Pipeline; parameter replacement
publishes a new416-byte existing GraphMaterialGPU record and updates prepared uses without compilation.

## Contracts and ownership

MaterialProgram is the runtime projection of the existing Cooked Unlit fragment/parameter contract,
not an author graph or compiler. The actual fixture uses MaterialGraph/lowerMaterial/ShaderEmitter and
glslang, with source/output/compiler hashes. It contains no runtime compiler link. Sixteen vec4 lanes and
eight Texture references come from the existing Cooked asset ABI; generic Graph/Shader has no such limit.
Native reflection validates the complete block and entry during cold Graph compilation. SetMaterial's
Success certifies the record upload/publication, not a newly compiled Effect; bound structural changes
are rejected preserving last-good. A different program uses a new cold candidate/Effect attachment.

Material textures are explicit TexturePublished content versions. SetMaterial selects a new version;
SetTexture alone does not mutate an already published immutable Material snapshot. Direct TextureBinding
continues its existing follow-current behaviour. TextureStorage::pin reuses the existing Backing/Use and
physical owner, never allocates another Texture/VMA record. Material's input may borrow Global or its
own actual StorageSet; inward references into shorter-lived unrelated Owners are rejected.

StorageSet owns the MaterialStorage. Registry only borrows it. Backing owns a BufferPermit and immutable
CPU/cooked state; DeviceSession remains sole Native allocation owner. Candidate owns the professional
transaction plus existing StorageLease and Transfer input bytes; no new upload state machine. Context Use
borrows prepared Buffer or pinned Texture/LastUse. Every physical version reserves Retirement before
publication. When future uses end, Texture pins and logical Storage leases are released before the
Material backing enters GPU retirement; Texture's own submitted LastUse protects its physical allocation.
The test destroys both logical Storage objects with a real accepted Draw still unobserved, then drains.

Optional Material resident budget counts its live/retired GPU record bytes; Texture allocations are
counted by Texture's single ledger. Command/Config accounting includes the entire retained cooked SPIR-V
capacity and entry string; shared owners are conservatively charged. Returned original requests and Code
Pins survive Client/Server destruction. No new SPSC/lifecycle/Request owner or device is introduced.

## Evidence and actual limits

Windows Release all-j4--k0, second no-work, CTest136/136134.30s. All116 prior tests retained plus17 real
asset dependency tests and3 Material tests. Final stronger closeout/stale Texture revision assertions plus
fresh relocated SDK consumer4/414.18s; final build no-work. New public headers compile independently.
Three Queue modes: actual Unlit params64/191 and sampled Texture96 pixels, exact Shader member/stride ABI,
one nonzero Pipeline creation, no Pipeline increase on params/Texture content update, two Contexts, failed
Submit rollback, stale revision/type, cancelled candidate generation, source/budget/CodePin late lifetime,
logical teardown before Fence observation, and record/execute first-party new0. Validation/Sync errors0;
actual duplicate EOS overlay warning retained. Pure Client/no Vulkan and sync/no stdexec remain tested.

Failures retained: first Native fixture used a nonexistent Registry erase (removed in favour of real Set
lifecycle); two-client fixture initially had unequal setup request counts (balanced setup, same-ID assert
retained); wrong UV location was rejected by Native Shader ABI then corrected to the existing emitter's
location3; invalid null offline Texture AssetId was rejected by existing Material validation and replaced
by a concrete fixture AssetId. A mistyped BC1 enum was corrected. No assertions/validation were weakened.

Logs/scripts: E:/RenderV2Rebuild/V12/evidence/N6-material/{tools,implementation}; runner
E:/RenderV2Rebuild/V12/run-material.ps1, Source=.../source Build=.../build/r Action configure/build/build/test.
Protection checker check_material_protection.py passes719 blobs and3 user files; old checkers unchanged.
Independent exact-I ASan/SDK qualification follows. N6.2 remains IN_PROGRESS until that checkpoint and
integrated BasicMesh consumer; Instance/Camera/BasicMesh/DepthOnly/N6.5 and N7 are not completed.
No complete MaterialGraph product/alpha blending/advanced materials claim. Historical performance
PARTIAL and product Root TOOLCHAIN BLOCKED remain unchanged. Upper Engine/Editor/Linux/Android/TSan NOT_RUN.
