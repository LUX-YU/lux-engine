# N2/N3 owned Handler and selection checkpoint

A separate HandlerId SlotMap uniquely owns each stable professional object. CommandEntry's
active {invoke, object} is a borrow; cold support entries reference that owner by HandlerId.
The member thunk accepts only the exact Request and move-only Reply signature with noexcept.
Reply construction is private to the qualified binding; malformed test inputs use test-only audit access.

Server::emplaceHandler consumes H::Bindings (a tuple of CommandMethod types), reuses type+instance
key, validates all full Command/TypeOps/ABI/SBO contracts before publishing any support, and
keeps source code pinned through the unique deleter. It is a cold provider primitive, not a
completed replacement for registerStorage/registerFeature. Those one-call installers still need
migration. No new Registry, thread, queue or per-request Handler ownership is introduced.

ListCommands/ListCommandBindings/SelectCommandBinding use the normal Command route. The first
support remains default; subsequent support does not steal active selection. Selection is visible
at actual dispatch, independently per Command. Owned response names survive Server destruction.
All deferred work ends before the Handler table clears, while DeviceSession is still alive.

A contract review corrected an interim behavior from the initial Reply checkpoint: dropping an
unsettled deferred capability now reports OUTCOME_UNKNOWN, not an unproven CANCELLED. Explicit
cancel still returns CANCELLED. The new test commits a visible side effect before dropping Reply;
it verifies unknown outcome, exact original input return, one destruction and full budget refund.
Historical checkpoint reports remain unchanged; their mechanical results do not supersede this
corrected semantic contract.

Full Windows RelWithDebInfo all -j 4 -- -k 0 and second no-work passed. The first 106/106 run
(185.12 s) preceded this correction; the corrected full 106/106 run passed in 144.81 s. Both attempts
are retained under E:/RenderV2Rebuild/V12/evidence/N2/handlers{,-corrected}. These durations are
not performance comparisons. The new handlers test proves four members/one object, two objects
sharing the Command, idempotency, rollback of a late conflict, FIFO switch, cross-Client coordination,
per-Command selection, invalid selection preserving active, and exact one-time destruction.
Legacy719 and all three user files remain unchanged. Measured active call 16/8, hot entry 24/8,
cold metadata 176 and RenderRequest 256 bytes. Independent ASan verification follows exact I.

Open joint-gate work: production one-call Native installers, dynamic capability refresh, unbinding,
FeatureUse typed configuration, removal of old registration/GPU classification and the specialized
Pending variant, professional Native operations and full stop/cancellation semantics. N2/N3 remain
IN_PROGRESS; no stage PASS, performance upgrade, push, N4 or later-stage work is claimed.
