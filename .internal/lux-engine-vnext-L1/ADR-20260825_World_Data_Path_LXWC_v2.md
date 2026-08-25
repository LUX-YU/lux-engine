# ADR: World data paths and LXWC v2 runtime loading

- Date: 2026-08-25
- Status: Accepted and qualified; independent re-audit required
- Baseline: `a2fef415`
- Supersedes: the LXWC v1 persistence portions of
  `ADR-20260824_Foundation_TypeInfo_Serialization_LXWC.md`

## Decision

World data uses four permanently separate paths:

1. `WorldSnapshot` is a same-process, same-build storage clone for PIE, tests,
   and coarse rollback. It has no portable byte representation.
2. `LXWC v2 WorldSection` is a disposable cooked runtime asset. L1 owns only
   structural validation and bulk load/unload into an existing `World`.
3. Persistent scene state is a future `PersistentEntityId` keyed delta over a
   cooked baseline. It is not a complete World dump.
4. Authoring/interchange owns editable data, migration and unknown-data
   preservation above L1.

Production `World -> cooked bytes` belongs to future L4/L5 Cook. L1 does not
provide a shipping writer, migration callback, AssetStore dependency or Scene
orchestration.

## L1 contract

`engine/ecs/world_section` becomes the installed runtime boundary. LXWC v2 is
an immutable owning byte image with component-column descriptors. Local Entity
identity is the implicit ordinal range `[0, entity_count)`. Persistent identity
is optional ECS data rather than an Entity directory requirement.

The format contains no archetype table or entity record table. Columns use
`TAG`, portable `FIXED`, or portable `VARIABLE` values and `DENSE` or sorted
`U32_LIST` membership. Runtime loading performs one erased call and one typed
storage lookup per non-empty component column, then static value decoding and
EnTT range insertion. Runtime complexity is `O(N + M + R + B)`.

`WorldSectionLoader` loads into an existing owner-thread World at an idle safe
point, including a World with a live Schedule. It suppresses per-row Change
Journal records. `WorldSectionLoadBatch` stages any number of load/unload
operations under one lexical edit and commits them in the fixed observation
order remove, destroy, create, add. A normal commit publishes exact changes and
does not mark history loss. If journal publication cannot allocate, canonical
commit remains successful and the batch marks one pin-safe history-loss epoch.
A failed transaction rolls back all staged canonical mutations and publishes no
change history.

`WorldSectionInstance` is World-identified and has explicit
`INACTIVE/STAGED/ACTIVE` lifetime. A staged instance cannot move, destruct or be
staged again; an active instance must be explicitly unloaded. It pins only code
owners used by its materialized columns. A section-local slot/generation
membership ledger tracks both loaded and gameplay-added Components, making
unload linear in section entities plus their current memberships without
scanning every World storage for every Entity.

LXWC representation widths and versions are format facts. Entity, column,
component-row and image validation limits, decode scratch, and Schedule change
scratch are caller-provided runtime policies. Schedule scratch is expressed in
change records, grows adaptively when uncapped, and preserves history-loss
fallback on allocation failure. This follows the repository rule: **Format
defines widths; Product defines capacities.**

`WorldSnapshot` is independently rewritten to traverse each COPY storage once.
Snapshot, WorldSection, persistent state and Authoring never share a universal
archive or registry.

## Ownership and dependencies

```text
L4/L5 Cook (future) -> LXWC v2 bytes
L2 AssetStore (future) -> immutable resident bytes/image
L3 Scene (future) -> safe-point load/unload ownership
L1 world_section -> core + schema + core::serialization
L1 persistence -> stable entity identity + world_section contribution
```

L1 does not implement L2/L3/L4/L5 in this phase. `persistence_contract`,
`WorldSectionWriter`, the LXWC v1 mutable object tree and all v1 compatibility
surfaces are retired without aliases or migration shims.

## Performance and freeze line

The committed LXWC v1 Writer has a confirmed `Theta(N^2)` duplicate scan and is
not repaired as a long-term path. It is deleted during the v2 image cutover.
The previous LXWC performance conclusion is withdrawn.

The v2 loader, storage-driven Snapshot, 1M scaling evidence and installed
consumer gates now pass. Qualification evidence is recorded under
`.internal/benchmarks/20260825-l1-world-data-path-v2`; thresholds are external
policy rather than L1 C++ constants. Current status is:

```text
Foundation Qualified
L1 Architecture Accepted
Correctness Hardened
Performance Contract Passed
Public API Freeze Candidate
Independent Re-audit Required
Domain Migration Blocked
```

Frozen still requires a separate independent-review acceptance commit.
