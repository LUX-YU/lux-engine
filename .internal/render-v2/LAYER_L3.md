# L3 — encoding, public visibility and one Server implementation owner

Parent: d707148fc (full parent in Git). Internal implementation gate; independent L4 remains mandatory.

Wire.hpp/.cpp is replaced by Encoding.hpp/.cpp; views explicitly distinguish raw encoded bytes from borrowed retainable owners. Both views borrow. OwnedWireBytes is removed in favor of lux-cxx SharedBytes. Encoding version/limits/functions and DataSchema specialization methods use the new names; byte format, field order, alignment/range/overflow checks and stable error ID strings are unchanged. Payload includes only SharedBytes and RenderError; Storage declarations no longer pull the encoder. Existing byte golden and malformed-input tests remain.

RenderServer.hpp no longer imports Client, builtin Commands, IpcCore or Native. Required typed Command transport templates remain installed. A single private ServerImpl.hpp owns the same members in the same order. Six implementation units separate bootstrap, registration, Scene, Effect, Completion and Loop. No helper owner/registry/thread was added. The external split-proof checks 34 moved member bodies token-for-token after the approved encoding rename and proves identical member order. Native Session construction/destruction now lives in DeviceSession.cpp; SessionUploads is the one component-private upload state definition.

The existing platform_process component provides cold OS entropy. Runtime has no Windows/BCrypt include/link. No third new component was created: this task adds only protocol_async and native_vulkan_extensions. Linux/Android implementation branches are NOT_RUN. The existing process argument test joins bootstrap, raising the total from 101 to 102.

Two IPC test consumers migrate the removed SharedBytes alias/header; their assertions and production IPC code are unchanged. All old reports and historical checkers stay read-only. Active Core/Data/Graph/Feature/Native/Runtime README files describe current boundaries instead of old stage pseudocode.

Public header inventory: 45 Render headers, with source closure probes for Descriptor, Storage, Payload, Native Compiler and Server. Independently compiled installed headers cover the new Data/Graph/Feature contracts. Fresh relocated installation rejects obsolete features/Texture.hpp, features/Program.hpp and data/Wire.hpp. Sync Protocol and Client disable stdexec lookup; Client also disables Vulkan.

Local validation: full all -j 4 -- -k 0 and second no-work. Full run l3-test-1-console.log: 100/102 passed; remaining failures were an installed Data probe still naming Wire and an installed Runtime consumer relying on the removed Client transitive include. Those consumers were migrated, without restoring the dependency or dropping assertions. l3-installed-2-console.log: all four Data/Graph/Feature/Runtime installation groups pass. All 102 tests are therefore covered; final L4 reruns the full matrix on exact I.

Current measured shapes: Message=144B/SBO=80B, Reply=304B, TypeOps=88B; RenderRequest=256B, DeferredCompletion=736B, Reservation=16B, Context borrow=32B, Permit=16B. Full small typed commands retain zero first-party allocation assertions; 128B values correctly use external storage. No performance-equivalence upgrade is claimed. Queue capacity=3 and sleep=1ms stay fixed. Root product closure and previous IPC/Alias performance qualifications are separate historical BLOCKED/PARTIAL.

External evidence: E:/RenderV2Rebuild/Layer/evidence/L3 and l3-*.log, including failed attempts. Final independent verification/push has not yet happened at this commit.

## Current format consumers (W05)

| Consumer | Lifetime / purpose | Current repeated work | Later gate |
| --- | --- | --- | --- |
| CreateEffect Feature config | Original typed Request retains payload and metadata pins through compile; assembly borrows | Complete schema/config validation then trusted feature-specific decode | W1 may replace in-process format with actual typed config; no change now |
| Storage config | Typed shared Config retained by registration; Factory checks actual C++ Config/Ops qualification | Schema is declarative, not a cast proof | W1 |
| Attach binding | Effect-owned encoded bytes, session-local resource IDs; Native prepares stable binding once | Registered schema validates bytes, professional Storage decodes its binding | W1; never persistent cooked assets |
| Texture/Buffer asset | RenderPayload retains complete backing and code pin through GPU work | No encoded Command or asset deep copy | Protected lifecycle |
| SPIR-V / ShaderInterface | Cooked stage data and GPU ABI; CompiledGraph owns immutable native program | Reflection/compatibility only in cold compile | Future capability gates |

Long-lived owners remain exactly the existing Runtime tables, ResourceRegistry, DeviceSession/VMA, Effect code and one deferred set. Host scheduler, immutable factory/config metadata and code modules outlive accepted work. Callback/operation and callback-code lifetimes are distinct and covered by existing async DLL-tail, stop and GPU tests; no GPU completion is inferred from a CPU event.
