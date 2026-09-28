# Editing

`history/` owns the single pure `edit_history` algorithm; `sessions/` owns the
`edit_sessions` identity, checkpoint and admission protocols. Their installed
includes, target names, library names and packages are unchanged by the P02-R1 move.
The source directories `editor/history` and `editor/sessions` are retired; do not recreate them.

The root `editor_editing` target and `scene/` remain old-product adapters until P12.
They are not part of the foundation scopes and cannot become dependencies of new models.
The private persistence bridge remains in `editor/transition`; it is not installed.

Scene authoring is in `editor/tools/scene/model`, using the same history and sessions.
Codec reads acquire the existing SessionState gate for their entire synchronous
operation. No long-lived lock or new busy state is attached to read views.

Historical receipts are immutable. Run `editor/tests/architecture/check_historical_evidence.py`
to validate them against their implementation Git trees rather than today's source layout.
