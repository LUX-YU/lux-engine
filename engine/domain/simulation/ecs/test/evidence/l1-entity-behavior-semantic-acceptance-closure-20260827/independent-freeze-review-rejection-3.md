# Independent freeze review rejection — semantic acceptance closure

Date: 2026-08-27 (Europe/London)

Reviewed evidence commit:
`d72cc8366d789533d00f8b6ed6a8eb81aa8017ca`

Reviewed production/test commit:
`dcb85fcb5fcf0124137e523b702885a2d2bb5fe0`

Decision: **independent correctness, semantic, and normative-contract
acceptance failed**. The build, performance, architecture direction, exact
Entity Event routing, Native ABI v2, executable identity, worker Event buffer,
and most previous-review closures remain useful historical evidence.

The candidate is rejected for these freeze blockers:

- ordinary EntityBehavior Hook bindings were incorrectly routed as
  Entity-targeted calls instead of flat Hook subscribers;
- lifecycle failure could move staged mounts out of rollback ownership;
- binding-only edits did not retire obsolete prepared methods;
- replacement published the new index before stopping the old instance;
- Lua component helper closures captured a deletable raw instance pointer;
- FlowForge overload edges resolved generated exports by diagnostic name;
- lifecycle reentrant ScriptComponent signals could be erased by `dirty.clear()`;
- authoring/FlowForge/Lua descriptors retained borrowed strings;
- Python dotted import aliasing duplicated module prefixes;
- Revision 2 no longer matched the uint32 StopReason, Native ABI v2,
  semantic catalog, sparse index, or benchmark v8 implementation facts.

The new normative source is
`doc/l1-entity-behavior-method-binding-quality-closure-refactor-spec-rev3.zh-CN.md`.
This rejection is audit input, not a normative implementation contract.

L1 remains NO-GO and formal L2 remains BLOCKED until a new production exact
SHA completes qualification and then passes a separate independent API and
semantic acceptance.
