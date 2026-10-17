# N2/N3 joint implementation checkpoint

This commit completes the remaining bootstrap and directory migration for joint independent review.
N2/N3 is IMPLEMENTED_PENDING_VERIFICATION, not a claimed gate PASS. N4-N7 remain NOT_STARTED.
Earlier exact-I checkpoint qualifications and failed attempts remain unchanged. Root product historical
BLOCKED, upper Engine/Editor NOT_RUN, performance PARTIAL; no push and no performance improvement claim.

Hello publishes only five discovery/control Commands. The Client explicitly consumes ListCommands and
refreshes its directory before binding business Commands. No automatic snapshot refresh or registry was
added. A runtime-private control without any selectable Handler cannot be unbound; installed professional
Commands retain their explicit unbind/selection semantics. This is a cold control boundary check.
ListFeatures owns the config type name/size/alignment and accounts for its full retained string capacity.
Its result may outlive Server/plugin metadata. Feature presets resolve stable names anew after restart;
reordered installation proves the new FeatureId is selected, and the old Session binding is rejected.

The unique Command SlotMap has a 16-byte non-owning active call; its stable cold metadata owns descriptions
and supported Handler identities. The separate Handler SlotMap owns each object once. Installation first
qualifies Command/factory/config contracts, then publishes without a recoverable partial state. First
supporter stays default; later support does not steal active/unbound selections. DeviceSession exists at
Server create, before installation. No run-time backend enum or second bindNative initialization remains.
Feature entries are stable; ConfigValue owns immutable typed configuration and pins its destructor code.
The worker borrows selected entries/request/device, never the whole mutable Server Impl.

Reply<C> owns one completion right. Synchronous calls never reserve Pending. defer reserves the sole
stable slot/event landing place and response budget before moving the source; Native reborrows SBO input
from the stable slot. Generic Pending contains request/operation/settlement, not CPU/GPU/Texture/Readback
variants. Professional Texture and Readback operations retain their real Ticket/Copy/Acquire contracts.
Shutdown clears all accepted work/ReceivedRequest and callback tails before Responder, Handler, Device,
Instance destruction. The event, SPSC, Queue, LastUse and retirement algorithms remain unique.

Joint behavioral proof map (independent execution pending at this exact I):
- T-CMD-01/02/06/11: runtime.storage_registration, features.operations, runtime.handlers; automatic member
  installation, four methods/one object, provider qualification, rollback, idempotence/different keys.
- T-CMD-03/04/07/09/12/13: runtime.handlers; default/switch/unbound/FIFO, two Clients, late explicit refresh,
  unsupported/stale selections preserve active. Runtime-private controls cannot be disabled.
- T-CMD-08: runtime.async now holds A's old operation, selects the alternate Handler through B, finishes
  B's new operation first, proves A still pending, then finishes A through its original state.
- T-CMD-05/10/14: pure protocol qualification/compile rejection, source call layout, runtime.async_cost.
  Cost samples are observations only, not equivalence qualification or a claimed speedup.
- T-IPC-01/02/03/09/10: typed_client, typed_batch, client_dll/server_dll/async_tail, lifetime and platform.ipc;
  full backing/capacity recovery, budget refunds, Unknown, Canonical storage policy and late Code Pins.
- T-IPC-04/05/06/07/08/11: runtime.async 0/1/16/128, synchronous start/reentry, defer refusal, burst events,
  callback tail shutdown, SBO promotion; GPU capacity rejection remains before accepted work.
- N2 Feature scope T-FEATURE-01/02/03/04/06/07/11: features.operations, runtime.programs, gpu_ready,
  features assembly/config and lifetime; stable direct IDs, owned config, restart re-resolution,
  dynamic growth, stale/config rejection. N5 live layout/parameter and implementation-update work is
  not inferred from directory tests. Conflicting replacement currently rejects and keeps the old entry.
- N3 GPU/readback: gpu_ready (3 topologies), texture_operations/rendering/progress, buffer_extension,
  native LastUse/failures, runtime.shutdown/offscreen; native upload success still waits real completion.

Current implementation all build and second no-work pass. Before the final two new cross-contract tests,
full 106/106 passed in 118.47s; after them focused async/handlers/installed 3/3 in 11.57s and Feature preset
1/1 pass. Exact-I full RelWithDebInfo/Debug/ASan and synchronous SDK checks are required next. Historical
106-test checkpoints do not substitute for that joint result. Runtime README corrected to actual API.
