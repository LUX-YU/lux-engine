# Workspace activities

Existing domain activities, relocated with their separate real targets and ownership.
Author state remains in authoring; workbench and application are consumers, never dependencies.

The full value, publication, recovery and migration contract is documented with the
[layout model](../../authoring/layout/README.md). WorkspaceStore owns IO and borrows the shared
WriteCoordinator; layout_model owns only values/codecs/plans and never links this activity.

`prepareLegacyMigration(LegacyWorkspaceInput)` converts owned, versioned bytes without filesystem or UI access.
`WorkspaceStore::prepareLegacyMigration()` captures those inputs from disk and calls this same converter.
The immutable plan retains every layout and takes recovery only from the explicitly selected snapshot.
Migration retries recheck captured versions before publishing through the shared coordinator; unchanged inputs
do not need reparsing. Publication status and catalog reads are independent operations.

Settings documents use the same `WorkspaceStore` physical-root/version boundary and the same
`WriteCoordinator`. `WorkspaceChanges::saveSettings()` keeps the accepted write and eventual outcome
after the editing page disappears; it alone acknowledges the coordinator. A page observes and later
acknowledges this activity's report. Settings publication does not rebuild the unrelated layout catalog.
Installation/launch sources are not writable. Missing is distinct from IO, malformed data and conflict.

`continueProfileMigration()` preserves every layout, preferences, recovery and conversion-marker byte
from an explicit project store in its user-project store. Startup supplies the root keyed by the
persistent project ID. A preparation marker pins the complete source digest before copying; the final
marker follows confirmed copies. `WorkspaceChanges::migrateProfile()` owns the bounded, one-at-a-time
publication sequence and consumes its own successful intermediate reports. Failed and Unknown writes
remain visible through the original coordinator. The source is read-only; incomplete source reads and
unmarked different destination bytes reject migration. Completed profiles retain later personal edits.
Older `.lux/editor` conversion reads the explicitly supplied project store and keeps its existing
selected-snapshot, provenance and idempotence rules. New work is blocked during migration, while
already accepted completion reception continues.
