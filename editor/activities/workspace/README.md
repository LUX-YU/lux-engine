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
