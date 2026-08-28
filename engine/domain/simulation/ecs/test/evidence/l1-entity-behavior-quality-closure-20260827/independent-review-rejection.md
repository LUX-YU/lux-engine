# Independent semantic review rejection

Date: 2026-08-27 (Europe/London)

Reviewed evidence commit: `2bf05234c920b1a86a195e97ec68fc50be1cb5a6`

Reviewed production commit: `393180e9a67e01c9e6863b4d0bde4a8849b05b0c`

Decision: **NO-GO**. The earlier qualification record remains in the repository as
historical evidence, but it is rejected as a freeze basis. L1 is not FROZEN and
formal L2 work remains blocked pending a new exact-SHA qualification and an
independent API/semantic acceptance.

The independent review found these correctness and contract blockers:

1. queued event occurrences retained borrowed call-frame pointers beyond payload lifetime;
2. Entity-to-MULTI-Hook binding was rejected instead of implemented;
3. non-void Event binding did not require `CONST_REF` payload passing;
4. Script primitive identities diverged between the semantic API and adapters/fixtures;
5. entity-targeted dispatch indexed only the entity slot and ignored generation;
6. Lua mixed business return values with the invocation ABI status;
7. unsupported Lua marshal types were rejected during invoke instead of cold prepare;
8. the ordering test did not exercise a real worker-to-safe-Hook production path; and
9. the installed consumer did not prove an installed export-to-invocation path.

The replacement contract and acceptance boundary are defined by
`doc/l1-entity-behavior-method-binding-quality-closure-refactor-spec.zh-CN.md`
(SHA-256 `843118EA52385E71BF3347F07D94D58A63D6B5B3DD95590A3EEE95950D05DB28`).

This rejection is intentionally not a new qualification claim. A future evidence-only
commit may mark the corrected implementation only as a Freeze Candidate until an
independent acceptance succeeds.
