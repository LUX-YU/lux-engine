# Render v11 S1/S2 — G0 freeze

Baseline: `98798da75a201933e6950ef5631444fc199e783a` (G11A-R V), checked against origin before construction.
Input ZIP SHA-256: `32e8e5077c4fb91191df6b50d556606c97b8ee191754241f9de055f4ace4e60b`.
All 21 manifest entries and ZIP CRC pass. Original 719 Legacy blobs, the user's two modified files and both dependency
commits are unchanged. Evidence lives outside source at `E:/RenderV2Rebuild/V11/evidence/preflight.json`.
The specific S1/S2 contract governs the stale architecture sentences about S1-only scope/per-stage I/V.

## Existing authority and commit points

- IPC Core owns Channel/Link and two SPSC rings per Requester. ReceivedRequest borrows its Responder until settled.
- Client owns its RequestId slots and retained-byte accounting; Reply/returned original input can outlive Client/Server.
- Server::Impl owns one Operation SlotMap, current compile/deferred states, Scenes/Views/Targets/Effects and one DeviceSession.
- DeviceSession owns one ResourceRegistry, allocation ledger, physical queues and retirement; Effect owns one CompiledGraph.
- GPU success follows actual submitted work and completed tickets/acquire. Accepted input and queue submission are not success.
- Current TextureUpload::advance overwrites old backing LastUse. This confirmed risk must be removed by monotone per-queue
  use recording before S2 can pass. Existing historical PASS records are not rewritten.

## Bounded changes, no additional authority

G1 replaces only message representation. Static TypeOps constructs/moves/destroys real C++ objects; an immutable typed
construction proof performs complete cold TypeToken/size/alignment/ABI validation against canonical static operations.
The handler boundary compares canonical qualification, not cross-DLL local thunk addresses or a hash alone. CodePin must
survive the destroy thunk's return, including a Reply kept after Server destruction. Prefer a direct CodePin member over
an extra owner hierarchy. Measure the 96B unpinned candidate and fully safe pinned/SBO alternatives; no target size is PASS
before measurement. G1 temporarily retains sufficient SBO for old 128B byte tests through one typed BytePayload; G3
removes production Wire consumers before selecting the final layout. This transition is explicit, not a second protocol.

G2/G3 replace the original table with Command entries and read-only BoundCommand. Protocol owns neutral type/command
contracts and the narrow non-owning invocation/completion function seam; Native never links back to Runtime. All twelve
built-in operations, three Texture resource operations, external Buffer and test Feature consumers migrate. Stats gets a
real small command. Feature/Storage configuration and binding Schema consumers remain. Request/Response dynamic owners
have explicit retained-capacity accounting. Failed submit restores the caller's request and refunds every reservation.

G4 reuses the host CpuScheduler through one cold template injection, with no modules-to-engine dependency. Existing
in-flight state becomes one bounded stable generational owner. CPU receivers publish only reserved Slot notifications to
one bounded lux-cxx queue; its short lock is not an IPC queue change. Sync start/reentrant completion and final callback
handoff must be proved before state reuse. PortSender's suspected synchronous completion UAF gets a reproducer before
any local correction. No Pending/atomic-done scan; no new pool/worker/request owner.

G5 reuses physical Queue poll/frontiers and Transfer. Intrusive ordered completion nodes live in stable operation state;
reserve before submit, register real accepted tickets, dispatch only at the RenderThread safe point (not inside begin/poll).
One upload(Storage&, Data&&) dispatches StorageTraits<NativeStorage>. Texture validation/candidate publication returns to
TextureStorage. A fixed three-physical-queue requirements value merges same-queue maxima and preserves different queues;
Backing/PreparedBinding/Retirement share this evidence. No completion callback overwrites a newer use. UploadState's
general staging pages/limits/query pool stay Session-owned when its old header is removed.

## Gates and protection

G0 snapshot -> G1 object envelope -> G2 command bind -> G3 S1 cleanup -> G4 CPU callbacks -> G5 GPU/upload -> G6 cleanup/V.
Each gate must compile and validate before dependent construction. Preserve all 79 historical test names/behavior, R2
SPSC/Link/credits/closeout, Graph/Effect/Registry ownership, real three-queue GPU paths and steady Engine new=0.
Keep fixed sleep, Root TOOLCHAIN BLOCKED and IPC R2 PARTIAL. No S3/G11B-G13, RHI, Editor/ECS migration or main merge.
Final I contains implementation/tests; independent no-alternates clone V adds evidence only. Default push is fast-forward
to codex/render-v2-rebuild after successful verification and rechecking origin.
