# Shared graph editing

`graph::GraphEdit` in the existing graph component stages structural and layout changes for both
`MaterialGraphEdit` and `FlowGraphEdit`. It does not own semantic payloads, domain history, a gate, or
an execution service. The domain transaction exclusively borrows its graph until preparation is
abandoned or its single commit completes.

Preparation lazily copies only the stores that change. Layout-only edits borrow the original topology;
an empty edit copies neither store. Node removal also removes its candidate layout. Placement requires
an existing candidate node and finite coordinates. Unplacement requires an existing candidate node,
but succeeds when that node has no placement. Domain type, function-reference, variable and export
checks remain in their original owners.

Issued NodeId and PinId values never recycle. Candidate issuance immediately merges the original
GraphTopology high-water marks, even if a later operation rejects the transaction or the caller
abandons it. This changes allocator state, not published nodes, pins, links, layout or payloads.
Explicit restoration still admits old identities; exhaustion is absorbing. There is one identity
allocator and no Session-side counter. A copied historical topology still needs the existing
`preserveIssuedIdsFrom()` reconciliation before replacing a live topology.

Commit swaps prepared pure values. It allocates no memory and calls no domain code. A domain commit
transfers its already-prepared payload storage in the same synchronous, callback-free interval.
Removed payloads remain owned by the domain transaction until its existing destruction or journal
handoff. Caller-owned Flow node and pin identities are assigned only on successful commit.

Use after commit or move is a contract violation. Moving a live edit transfers the exclusive borrow;
copying and move assignment are disabled. Abandonment never publishes a candidate. Semantic errors
are returned by the preparation operation; ordinary heap exhaustion retains the project fatal policy.

The real installed SDK regression records the former identity reuse in both domain transactions.
The shared edit tests additionally cover candidate restoration, direction and duplicate-link errors,
layout locality, high-water exhaustion, and explicit restoration after exhaustion. This is the
structural transaction extraction only: replacing legacy polymorphic domain nodes, pin payload lookup,
codec/compiler integration and the remaining Flow extension catalog are separate, unfinished work.
