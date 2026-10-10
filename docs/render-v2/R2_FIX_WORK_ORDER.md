# R2-FIX — Reply lifetime closure and architecture amendment

User authorization dated 2026-10-10; verified remote/local base
`2e09985290ee630af0b524bd55d100c02c424d57` on `codex/render-v2`.
The user explicitly approves bounded pre-reserved reply cells in place of V1
pending reply publication/retry, and requires consume/abandon/recycle closure.

Allowed production: `modules/function/render/transport/**`. Supporting changes:
architecture chapters 03, 05, 07, 09, 10 and `docs/render-v2/R2_FIX_*`; bootstrap
only if needed for R2 validation. Core, frozen Legacy, historical R0/R1 reports
and `R2_VERIFICATION.md` remain read-only. No Graph, Vulkan, Runtime, Feature,
Scene/ECS, Editor/UI or root/product CMake implementation is authorized.

Use explicit `RenderReplyInbox::abandon(ticket)`, retaining lightweight numeric
tickets. Cancellation leaves a readable terminal result; the receiver must take
it or abandon. Pending/terminal abandon recycles; WRITING abandon transfers
reclamation to the writer, which finishes byte access before advancing generation.
No global ticket registry, per-request heap or additional shared ownership.
Review foreign/stale/duplicate requests and completion/cancellation/stop races.

Keep all 23 R2 tests and ten benchmark cases. Add single-slot lifetime recovery,
terminal abandonment, deferred/late completion, deterministic WRITING races,
stop races and sustained capacity reuse. Run same-machine before/after against
the frozen R2 benchmark, preserving raw trials and zero-allocation checks. Stop
qualification on unexplained p50/p95 regressions over 5%.

Amend only the specified authoritative chapters with Simulation/Publication/
Rendering separation, independent render progression, distinct simulation and
render time, reusable graph plans and dynamic bindings, paced execution and
R3/R5/R7/R8 gates. No future production framework or frame loop is implemented.

Commit implementation I, validate tracked inputs, qualify an independent clean
clone, archive external logs/hashes and write a separate report-only V. Preserve
the original user checkout and synchronize public headers to three prefixes.
Push and STOP. R3 requires both R2 qualification and explicit user review approval.
