# N2 live command directory prerequisite checkpoint

N2/N3 remains IN_PROGRESS. This checkpoint removes manual Command versions, the duplicated Capability
record and cached Server manifest. The unique Command table now builds directory replies at the Owner
safe point. Clients explicitly refresh their discovery cache; malformed refresh leaves the previous
cache and existing immutable BoundCommand qualifications intact. A null selected Handler deliberately
unbinds a Command; a later supporter never steals that selection.

Tests cover live installation, two Clients, FIFO selection, stale Handler identity, explicit refresh,
wrong-session/directory failures, duplicate names/IDs, canonical type/layout validation, original Reply
budget refund and exactly-once Handler destruction. RelWithDebInfo full all / no-work / CTest passed
106/106 in 224.79 s. This is implementation evidence, not independent checkpoint qualification.

The only IPC edit is the existing protocol fixture's obsolete version mismatch replaced by an actual
response type mismatch. Production IPC is unchanged. check_directory_protection.py preserves the old
checker and permits precisely these two fixture lines, retaining all 719 Legacy blobs, dependencies,
history and all three protected user files. Command schema versions are removed; durable Data schemas
are not changed. Hello still includes the initial directory at this intermediate checkpoint; final N2
must narrow bootstrap and migrate explicit discovery consumers. Old public registration aggregates,
Factory installation transaction, Feature selection and ListStorages remain unfinished.

Raw implementation evidence: E:/RenderV2Rebuild/V12/evidence/N2/directory and
E:/RenderV2Rebuild/V12/directory-{build3,nowork,full}.console.log. Failed build1 is retained: an old IPC
fixture used the removed Command version and was migrated as described above. No performance claim,
no final N2/N3 PASS, no push. N4-N7 NOT_STARTED; historical performance PARTIAL is unchanged.
