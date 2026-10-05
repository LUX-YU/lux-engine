# Content factories and installation

`SessionLoadJob` owns a fixed VFS snapshot, input identity and factory/code handle. Workers only read and decode owning data. `SessionPreparation::prepare()` runs on the Store owner: reserve an identity, construct the real domain session, transfer it to the Store's hidden slot, and prepare its history/save roles. Scene, Material and Flow instantiate one small constrained helper; each keeps its existing codec, model, gate and persistence implementation.

Factory registration does not resolve services or construct metadata. `SessionLoadJob::prepare()` runs at
request admission on the owner, under the service catalog read scope. A declared factory can resolve only
its declared dependencies and returns an owning decoder with fixed immutable inputs. The resolver and
ServiceScope do not reach the worker. Rejected candidates are destroyed before the read scope ends;
publication, dependency, scope and thread failures preserve their original structured cause. Prepared
jobs retain their input after service retirement and use the same read/decode and installation path.

Admission is checked before consuming accepted decoded input. BUSY, wrong-thread and capacity failures preserve the input for later owner delivery; they do not re-run the decoder. Once preparation starts, rollback releases roles before abandoning the hidden slot. An incomplete installation is never discoverable through Store or SaveService.

`PreparedSessionInstallation::publish()` checks the original owners and commits only prepared, non-allocating, non-callback state changes. `InstalledSession` owns role registrations and their code, **not** a second session. SessionStore remains the sole logical content owner; shared references retain the same allocation without extending a closed SessionId. Close uses the original content stamp and gate; a refusal retains the installation. A published session survives notification failure. Callers must explicitly close installed content before destroying Store/SaveService.

History observation is read-only; undo/redo invoke the domain's existing history. Save, Save As and Export Copy use the original SaveService and source roles. Existing operation IDs, checkpoints, identity high watermarks, publication facts and reliable completion delivery are unchanged.

The activity module declares `kSessionStoreService` and `kSessionOpeningService`. Registration alone
allocates neither. Opening retains the actual shared Store/SaveService allocations until accepted work
and installed role bundles are released. The pure Store does not acquire a services or Process dependency.
At its existing dynamic factory boundary, Opening borrows the composition registry/scope explicitly;
each content descriptor still resolves only its declared dependencies, after admission and deduplication.
The worker never receives the registry, scope, resolver or a live session. Scope maintenance calls the
same update algorithm. Domain close remains explicit; releasing references is not a discard decision.
