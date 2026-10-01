# Workspace activities

Existing domain activities, relocated with their separate real targets and ownership.
Author state remains in authoring; workbench and application are consumers, never dependencies.

The full value, publication, recovery and migration contract is documented with the
[layout model](../../authoring/layout/README.md). WorkspaceStore owns IO and borrows the shared
WriteCoordinator; layout_model owns only values/codecs/plans and never links this activity.
