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

`continuePreferencesMigration()` copies the existing selected-layout/opaque/provenance values from an
explicit project store into an explicit user-project store. The caller uses the persistent project ID
to choose that root, settles each returned ticket, and calls again to publish the success marker. The
source is read-only. Unmarked different destination bytes conflict; after the marker, personal edits
remain authoritative. Layout and recovery contents are not interpreted or combined by this operation.
This API does not select the application's profile root; startup composition supplies it.
