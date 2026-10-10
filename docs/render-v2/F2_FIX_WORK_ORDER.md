# F2-FIX — Scope/share correctness repair

Authorized base: `3216bc8d19567abf8fc00c76bab7fdb3fff77e1a`; F2 implementation `f2318d708609e686ed49bdf61de1bade9b17279e`; frozen reference `a669409a289a6fa4092f21176397795b1cdb7f3e`.

The user's 2026-10-10 F2-FIX work order authorizes only this repair. The exact input and its hash are preserved in the external preflight evidence. Historical F2 PASS remains unchanged. Final qualification has not occurred merely because this work order is committed.

## Scope

Allowed: Graph production/tests, `docs/render-v2/F2_FIX_*`, necessary bootstrap verification. Two exact conditional files implement the single neutral role classifier: `modules/resource/description/include/lux/engine/description/PassContract.hpp` and `engine/toolchain/shader/src/PassValidation.cpp`. Their enums, schema layouts and generated declarations remain unchanged.

Readonly: Core, all Transport (including the previously approved alignment), R4 Vulkan, all 719 frozen source files and guard, FINAL, historical reports, original user worktree's six dirty files, Root/product, Scene, Editor, Runtime, Native Graph and business Features.

## Required work

1. Reproduce live View → ORDER → Scene false acceptance against the baseline production library before modifying C7. Preserve source, compiler command, binary hash and failing result.
2. Separate execution ordering, producer liveness and share prerequisites. Validate all live execution predecessor paths conservatively; reject narrowing Scope with precise diagnostics. Never make ORDER a data-liveness edge.
3. Compare every proved Scene predecessor's actual invocation parameters/import evidence, including conditions. Preserve independent Scene sharing across Cameras. Preserve C3 versions, producer selection, range partition and fallback validation.
4. Extract bounded stateless algorithms, centralize role semantics, rename the diagnostic digest so it cannot advertise structural cache identity. Exact typed compatibility remains authoritative.
5. Retain 116 test obligations; add Scope, hazards, condition/culling, role and digest cases. Measure 64/128/256/512-pass mixed-subresource cold compilation and unchanged million-binding paired workload. No semantic reductions for speed.
6. Commit I, qualify an independent clean clone with ordinary/full ASan all builds/tests, actual dependency/codegen closure and protection checks; V adds only the new qualification report. Push and STOP.

Authoritative grounding: FINAL 04 §4.5–4.7, 06 C4–C8, 12 value/algorithm/scratch boundaries, 13 error/borrow/cold/hot contracts, 15 F2 scope, 16 regression evidence, 17 I/V isolation and 19 D03/D04/D18–D20/D27–D31. This repair concerns I02/I34 logical correctness, not F3/F4 native execution.

Final claims require independent evidence: `F2-FIX PASS` only if P0 passes; each P1 item is reported individually. `F3-PRE-01` and `F3-PRE-02` stay OPEN, `Native RenderGraph = NOT_IMPLEMENTED`, `V2_PRODUCT = EXPECTED_UNAVAILABLE`, `F3 = NOT_AUTHORIZED`.
