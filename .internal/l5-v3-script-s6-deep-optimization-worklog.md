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
