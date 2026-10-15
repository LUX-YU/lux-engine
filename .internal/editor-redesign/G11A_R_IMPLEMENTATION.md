# G11A-R construction record

Baseline: `d5b4ff0ad792dd8e2d9ada7c26bda3c86fc33959`.
This is an internal construction record, not final I/V qualification.
R0 decisions and protection paths are in `G11A_R_ARCHITECTURE_FREEZE.md`.

## R1 wiring checkpoint

Added one neutral `render_resource` component. Existing Storage identity/ABI/Scope declarations moved
there; Feature, Native and Runtime consume that one definition. Descriptor validation compares complete
identity, policy and DataSchema. Native factory metadata carries the selected API, matching professional
Ops, configuration compatibility callback, actual creation callback and code pin. No physical instance is
created during registration.

Cooked Program moved to Feature and TransferOptions to Resource, without forwarding aliases. Graph Range
moved to a real PRIVATE include tree. Runtime public-header probes no longer add the native source include
directory. Public Runtime still has the old explicit native operation methods pending R3/R4 migration;
this checkpoint does not claim final public API convergence.

Texture upload/region/release now enter the original Operation SlotMap through `registerStorage<TextureStorage>()`.
Deleted the zero-pass Texture Feature and its registration header. `registerFeature<T>()` retains the same
existing dynamic registration implementation. A shared cold handler validator checks Schema/completion
contracts and duplicate names before either registration mutates the operation table.

Device capability negotiation is now driven by registered Storage required/optional capabilities, not Host
upload callback presence. Device selection records the optional capabilities actually supported by the
selected physical device; Manifest publication follows that negotiation. The old eager DeviceSession Texture
owner remains solely as the explicitly bounded R1→R2 wiring checkpoint allowed by the contract. Registry,
domain and lazy physical ownership are NOT_IMPLEMENTED in this checkpoint, and must replace that owner in R2.
Factory creation is the same fallible native Texture create function used by the current path; the isolated
second-API fixture only checks non-GPU factory/config/typed Ops semantics.

### Validation and actual failure retained

Commands, run in the isolated source using the external `E:/RenderV2Rebuild/G11AR/run.ps1`:

```text
run.ps1 -Action configure
run.ps1 -Action build             # all -j 4 -- -k 0
run.ps1 -Action build             # ninja: no work to do
run.ps1 -Action test -Filter .    # all 74/74, 83.55 seconds, Release
```

All original 72 names remain; new tests are `render.resource.contract` and
`render.runtime.storage_registration`. Existing installed/relocated consumers and independent public-header
compilation passed in that all build/test. This is implementation-environment Release evidence only;
Debug, ASan and final independent clone verification remain outstanding.

Initial failures were preserved, not reclassified:

* A generated header probe named `Storage.cpp` shadowed the new executable source in CMake. The probe now
  uses `headers/`, and the executable source is explicit. No test was disabled.
* The installed Feature dependency assertion still listed the old three components. It now explicitly
  includes the actual new `render_resource` dependency, retaining the unrelated-target rejection.
* The unchanged G10 lifecycle fixture intermittently awaited a submit which had already returned an
  explicit Native queue-capacity failure. Diagnostics reproduced `outcome=FAILED` and
  `error=7356206049137219893`, exactly `vulkan::kCapacity`, before any submit for sample 1021. The analogous
  stop sample 1022 used the same invalid assumption. Evidence: `r1-stop-reproduction.log` and
  `r1-stop-reproduction-2.log` in the external evidence directory. The final fixture consumes and records
  only explicit pre-submit capacity refusals, retries within the unchanged 15-second deadline, then performs
  all original abandon/stop/source-owner assertions after observing a real accepted Submit. No queue size,
  production GPU algorithm, observation hold, timeout or ownership assertion was weakened.
  Twelve consecutive same-queue GPU-ready runs then passed (83.97 seconds total), followed by full 74/74.

Protection checker passed: 719 frozen blobs, both actual user hashes/status/HEAD, dependency versions and
permitted change boundaries. Historical reports/checkers and IPC source remain unchanged.
Raw logs and snapshots are under `E:/RenderV2Rebuild/G11AR/evidence/`; final immutable archive/index is due
after final I. No final I/V, architecture PASS or push is claimed here. G11B–G13 remain NOT_STARTED.

## R2 checkpoint — actual unique instance ownership

DeviceSession now owns one ResourceRegistry. Its former `textures_`, `scoped_`, ScopedBuffer and
ScopeIdentity owners are removed. BufferInitializer and the FeatureRegistration buffer list are removed:
the former zero-buffer consumer installs a real Native factory through the same Storage registration.
Storage instance identity is Domain SlotKey + complete Type identity + semantic key. Domains use SlotMap
generation and a shared epoch token (not a second generation or a GPU owner); old/cross-session domains
are rejected before lookup. Root/Scene/View/Attachment parent chains are bounded to the four legal scopes.

Typed access returns the matching immutable Ops and instance borrow, with ancestor results read-only.
Config reuse is checked by the selected factory; new instances publish only after successful creation.
Domain close removes admission and generation first, while prepared Context/Upload references retain
instances until their actual GPU use finishes. Instance deleters run outside SlotMap mutation, allowing
professional indirect references to release another instance without re-entering container erase.
Factory code pins outlive their instance deleters. Scene and View store Domain values; the four-field
ExecutionGroup is unchanged. Context owns its Attachment domain and resolved bindings. Failed attach
rolls back only newly created instances and preserves earlier shared instances and last-good bindings.

Texture registration no longer creates GPU Storage in run(): first Root upload creates it lazily. Config
is supplied by `registerStorage<T>(T::Config)`, not Texture-specific Server capacity fields. The built-in
Native factory is selected at DeviceSession construction. Per-frame Context use caches its native binding;
Registry maintenance traverses prepared callbacks without Type/name lookup. Transient allocations remain
Context-owned and outside persistent Storage. Cold Texture helper APIs and the specialized upload work
remain explicit R3/R4 migration items, not claimed final APIs.

Verification in implementation Release: all build, second `ninja: no work to do`, full CTest **76/76**
(72.85 seconds). Log `evidence/test-RelWithDebInfo-20261015-162600-399.log`. Original 72 names remain.
New tests `render.resource.registry` and `render.native.resource_domains` prove scope/config/epoch
rejection, read-only inherited access, indirect-reference exact destruction, six actual Native allocation
failure points with empty rollback, last-good reuse, and domain closure while completion observation is
held after real GPU Submit. The existing texture-storage test additionally closes Root after accepted
upload work and verifies physical backing survives until completion and is destroyed on last work release.
The existing two-Scene/four-View, texture shader/color, queue, owner, installed and hot-allocation tests pass.
Public Registry/Native StorageBinding headers compile independently. GPU validation in affected tests is
zero error; Debug/ASan and independent final I/V are still due. This checkpoint is not final G11A-R PASS.
