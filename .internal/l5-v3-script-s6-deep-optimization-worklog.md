# S6 deep optimization work log

- B0: `a577c49409e2519029693fbe780fe8ce3ab2dc1e`.
- Implementation: `codex/s6-deep-optimization`, isolated worktree.
- Main's four pre-existing edits are excluded and untouched.
- Status: IN PROGRESS; no qualification or freeze claim.
- W0 precedes W1-W5. Bsafe is assigned only after W0 runtime regressions pass.
- Scope: LuaJIT JIT-on/interpreter; no PLAYER, EDITOR, Lua54, Linux or Android qualification.
- Final evidence must account for C01-C05, M01-M13, R01-R09 and X01-X02 separately.

## W0

- C01 failure-injection executable counts and suppresses duplicate physical arena frees, so the negative
  detects double ownership without deliberately executing an allocator double-free.
- Baseline C01 standalone MSVC reproduction (metadata failure 2): exit 1, arena_deletes=2, expected=1.
  No undefined double-free was executed. Production fix gives one owner throughout fallible metadata preparation.
- W0 Developer RelWithDebInfo/LuaJIT all-j4-k0 passes; full CTest 205/205 (33.08 seconds).
  Logs: `build/RelWithDebInfo/deep-developer/w0-all-final.log`, `w0-ctest-final.log`.
- First complete regression exposed five tests asserting the old pending-as-error result, plus the new Scene
  fixture's unchanged two-instance capacity for three authored mounts. Assertions now require success for pending;
  stale completion, exact destruction, waiter/continuation cleanup checks remain. Fixture capacity is explicitly three.
- C02 also covers a pending candidate preceding a failing BeginPlay, pending plus active gameplay, later
  rematerialization and final Scene-derived propagation with an indefinitely unresolved mount.
- C03 checks only this stage's prepared lanes (including a writer illegally surviving task return), before the
  task completes. The region producer returns void in the overflow regression.
- C04 accepts only noexcept void/bool Hook results. Command details survive cleanup and outrank generic system
  failure; simultaneous system reports select the minimum stable SystemInstanceId, not first-thread arrival.
- No W1-W5 implementation or performance qualification is claimed by this W0 checkpoint.

## W1 storage implementation checkpoint

- Bsafe is `b03cd8c043773565a79a4de6a91d0a6b3fbe6fa7`. Its binaries and build inputs are archived separately under
  `build/RelWithDebInfo/deep-bsafe`; benchmark EXE SHA256 is
  `686E7B8ABFAE74139BDD9A8821964703EDBE7C770EF12027145FDFD78F709F7E`.
- BoundedClassStorage replaces first-fit/coalescing storage. Prepared class -> nonfull page -> free slot is bounded;
  release returns to the original page without scanning or moving global free ranges. Class selection is at most
  64 layouts, performed in prepare for Native/Lua and against the actual allocation request for C++.
- Pages may be reclassified only by explicit cold maintenance and only while empty. A pool-local monotonic allocation
  stamp never resets during page reclassification; exhaustion closes allocation, but live releases remain legal.
  Generation exhaustion, stale release after reuse/reclassification, mixed size/alignment, move and all four factory
  allocation failures are covered. Arena, page/slot/class metadata and the storage object itself are budgeted.
- Native modules share the explicitly provisioned state classes. Fixed 128 live instances over 1/8/32/64 logical
  module assets produce the same 8192 state bytes. The fixture executable is shared deliberately: the old allocator
  duplicated its state slab per module-cache entry even for these distinct asset identities. No claim of 64 distinct
  generated machine-code images is made.
- Native continuation methods select a class once in prepare. C++ frames and owned-reference blocks use the same
  owner-free primitive in their existing descriptor owner. No heap fallback is provided.
- Lua prepared blocks preserve artifact-local contiguous indexing. A prototype selects its class once. An instance
  owns only its blocks; release visits its entries and a fixed number of allocator records, not global free spans.
  Class plans and byte budgets are explicit create inputs, not inferred from catalog population.
- Actual Lua backend structural fixture: 256 catalog methods, 2 prepared Ability entries + 1 Event entry per object;
  10k/50k/100k objects, alternating retirement/rebuild and random final retirement. Prepared bytes are respectively
  1200560/6000560/12000560; acquire/release indexed-record visits are 90008/450008/900008 each. No remaining entries.
  Every individual instance retirement incurs <=12 allocator indexed-record visits. Fixed pool control/stat updates,
  VM operations and entry destruction are distinct from this counter; it is not a CPU-load counter or total VM memory.
- A single 100k C++ coroutine start/resume/destroy structural pass completes with zero Engine C++ allocations.
  Timing from this one diagnostic pass is not a performance baseline or a before/after speed claim.
- Developer all-j4-k0 and full 206/206 CTest pass before the additional alignment contract test. New tests replace
  the retired allocator's tests; tests asserting its old first-fit behavior are not kept as a production fallback.
- Actual MSVC 19.44 coroutine probe: allocation receives size (240 bytes), not an alignment argument. Generated code
  performs its own extended-alignment adjustment and saves the original allocation pointer for delete. A 64-aligned
  local survives suspension with a stable aligned address. The production C++ bridge fixture tests the same case
  together with an owned const-reference argument. These are Windows/MSVC findings, not cross-compiler claims.
- Remaining W1 qualification: broader backend layout/quota cases and clean installed/Toolchain closure. W2-W5,
  all R/X decisions and final performance/qualification are still outstanding at this checkpoint.
