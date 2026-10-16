# N2/N3 initial typed Reply and hot/cold route checkpoint

Reply<C> is a move-only one-shot capability. defer reserves the existing bounded stable
completion slot before moving the sole RenderRequest. A successful promotion invalidates
the old inline request borrow; callers reborrow through the returned Reply. Rejected
promotion preserves input, capability and budget. Dropping a deferred Reply cancels it.
Completion clears the capability before publication and does not access it on success:
the event consumer may already have destroyed an enclosing operation. Existing publisher
tail and source-module pin guarantees remain. Sync requests allocate no Pending slot.

CommandEntry stores the active non-owning two-pointer call plus a stable cold record.
RenderRequest borrows the stable metadata's code pin; published owned replies take their
own pin. All ReceivedRequests finish before the Server metadata is destroyed.

Windows RelWithDebInfo full all -j 4 -- -k 0, second no-work and 105/105 tests passed
(183.20 seconds). Exact output is under E:/RenderV2Rebuild/V12/evidence/N3/reply.
Tests retain prior 0/1/16/128, producer-tail, DLL and GPU cases; new assertions prove
explicit inline input promotion, cancellation owner return, byte-capacity rejection,
exact destruction and budget refund. Legacy719 and three user modifications are unchanged.
Measured x64 ABI: HandlerCall 16/8, CommandEntry 24/8, cold metadata 144,
RenderRequest 256 bytes. These are sizes, not a performance qualification.

This is an intermediate checkpoint, not the joint N2/N3 gate. Old CommandRegistration,
GpuAdmission and CPU/GPU/Readback condition variant remain for the next continuous
migration. Handler ownership, typed installation and professional operations are not
claimed complete. Independent ASan validation follows this exact commit; no push yet.
