# Architecture convergence R2 implementation

Parent: `6becd3a8aca68d73395b8b9329638fd3fc9b25e2` (R1).

IpcMessage now uses mutually exclusive 128-byte inline storage and a single exclusive owned carrier.
tryAssignInline rejects only sizes over 128 without mutation; tryAdopt rejects null owners or more than four
attachments without consuming either input or destination. Inline move copies only initialized bytes; owned
move transfers stable owner/descriptors. Overlap, self-move, owned-to-inline overlap and array deleters are tested.
The ABI changes: Message 176 -> 144 bytes; Reply 360 -> 296 bytes. All consumers must rebuild together.
For default depth 64, the two rings' element storage falls from 34,304 to 28,160 bytes per Link; queue metadata
and allocator overhead are excluded. The entire Ipc.cpp prefix/suffix outside message methods is unchanged.
No SPSC, Channel, Link, publication, reply credit or reclamation algorithm changed.

RenderCompletion is a move-only value owning IpcReply directly. tryReceive transfers one completion, or processes
at most eight abandoned records. Active output cannot be overwritten. Pending stores correlation/abandon metadata,
never a result cache. Published replies precede CLOSED; remaining requests become OutcomeUnknown. Request SlotKey
reclamation follows actual consumption. Returned input retains the full backing budget even after Client/Server
are destroyed. No shared Reply wrapper or synthetic byte owner is made during ordinary completion consumption.
The old poll(id), collectReplies and RenderReply APIs are removed. Sequential legacy fixtures use only a test-local
receiveExpected assertion, which never caches unrelated replies. Stop/readback tests consume either arrival order.
The fixed 168-byte control record and its request encoding allocations remain until R3; R2 is not a claim of
allocation-free RenderClient API. Runtime neutral completion now publicly depends on IPC's message value, not Native.

## Implementation evidence

Serial commands (runner: E:/RenderV2Rebuild/ConvergenceR1/run.ps1):

- `cmake --build <build> --target all -j 4 -- -k 0` twice: no work on second build.
- `ctest --test-dir <build> --output-on-failure -j 1 -R "^(render|platform)\."`:
  RelWithDebInfo 58/58; ASan 58/58, including live Vulkan/SyncValidation tests and relocated consumers.
- ASan flags `/fsanitize=address /Zi`, `ASAN_OPTIONS=alloc_dealloc_mismatch=1`.
- Protection checker: Legacy 719, all non-whitelisted blobs, original user hashes, IPC algorithm regions PASS.
- Two intermediate installed IPC checks failed because the consumer still asserted old ABI sizes. Corrected
  both assertions to the measured ABI; subsequent full Release/ASan matrices pass. Failure logs are retained.

Three serial alternating cost runs used the exact prior independent I executable and the rebuilt R2 executable;
10,000 samples per case, same x64 MSVC host. Raw CSV/logs and per-round summary:
`E:/RenderV2Rebuild/ConvergenceR1/evidence/R2-ipc/`. The source fixture measures actual scalar encode/decode and
checks every payload. Separate preencoded queue samples are not substituted for complete operation costs.
No EXE or IPC DLL hot allocation in inline IPC cases. Whole RenderClient API measurement belongs to R3/R6.

| Case | Before p50 range | After p50 range |
| --- | ---: | ---: |
| Full codec, same thread, 8 B | 200 ns | 200-300 ns |
| Full codec, same thread, 32 B | 300-400 ns | 400-600 ns |
| Full codec, same thread, 64 B | 400 ns | 500 ns |
| Full codec, same thread, 128 B | 700 ns | 600 ns |
| Full codec, two physical cores, 128 B | 1300-1400 ns | 1200-1300 ns |
| Complete small test operation, same thread | 200-300 ns | 300 ns |
| Complete small test operation, two cores | 800-900 ns | 800-900 ns |
| Preencoded queue, same thread | 100-200 ns | 200 ns |

There is a measurable small same-thread cost increase in 32/64-byte cases, so this is a memory/ownership
improvement, not a universal latency win. No unsupported attribution or speculative SPSC optimization is made.
128-byte p99 before 1000/1600/1500 ns, after 1900/1200/1200 ns: tails do not show a uniform improvement.
Large-owner 1 MiB/256 MiB pointer identity and exactly-once destruction pass. Large handoff and release timings
remain separately recorded; release is on the consuming caller. Historical IPC performance PARTIAL stays intact.

R2 implementation gates pass; final independent Debug/Release/ASan batch V remains pending. Linux/Android/TSan
NOT_RUN; no GPU timestamp result is claimed. Root product closure and future R3-R6 work are not qualified here.
