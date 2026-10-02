# Content factories and installation

`SessionLoadJob` owns a fixed VFS snapshot, input identity and factory/code handle. Workers only read and decode owning data. `SessionPreparation::prepare()` runs on the Store owner: reserve an identity, construct the real domain session, transfer it to the Store's hidden slot, and prepare its history/save roles. Scene, Material and Flow instantiate one small constrained helper; each keeps its existing codec, model, gate and persistence implementation.

Admission is checked before consuming accepted decoded input. BUSY, wrong-thread and capacity failures preserve the input for later owner delivery; they do not re-run the decoder. Once preparation starts, rollback releases roles before abandoning the hidden slot. An incomplete installation is never discoverable through Store or SaveService.

`PreparedSessionInstallation::publish()` checks the original owners and commits only prepared, non-allocating, non-callback state changes. `InstalledSession` owns role registrations and their code, **not** a second session. SessionStore remains the sole content owner. Close uses the original content stamp and gate; a refusal retains the installation. A published session survives notification failure. Callers must explicitly close installed content before destroying Store/SaveService.

History observation is read-only; undo/redo invoke the domain's existing history. Save, Save As and Export Copy use the original SaveService and source roles. Existing operation IDs, checkpoints, identity high watermarks, publication facts and reliable completion delivery are unchanged.
