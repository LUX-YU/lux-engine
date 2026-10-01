# Editing

The shared `editor_contracts`, `edit_history` and `edit_sessions` targets own the pure identities,
history algorithm, SessionStore and SessionState. They share one physical public include root,
but retain their binary identity owners and separate installed packages.

ViewInfo is a pure observation; window errors and close preparation belong to view_api.
EditorError is a pure value supplied by editor_contracts.

The `editor_editing` target, EditHistoryTarget and transition/LegacyPersistenceState are retained
only for the existing product until P12. They are not dependencies of the new authoring or activities.
