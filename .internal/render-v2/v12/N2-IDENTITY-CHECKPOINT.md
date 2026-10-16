# N2 identity/directory checkpoint (N2/N3 remains open)

This commit separates stable FeatureName/StorageName/StorageNameView from runtime FeatureId,
StorageTypeId and StorageId. StorageId reuses the old instance SlotKey directly; no Ref, extra
generation, alias or new allocator was introduced. ResourceRegistry's type directory now uses one
StableSlotMap and stores its generated type ID in instance records instead of a vector offset.
Factory/configuration addresses remain stable as types are added. Instance ownership and the
Domain hierarchy are unchanged here and remain N4 work, not a qualified final owner model.

The Server Feature directory uses StableSlotMap<FeatureEntry>. Registration returns its actual
FeatureId and ListFeatures returns owned names plus IDs through the normal typed Command path.
The test crosses 600 entries, compares every registration ID with the actual Client response,
and reads the response after Server and source metadata destruction. A separate 600-type Registry
test checks stable factory addresses, stale generation rejection, missing names and zero physical
resource creation. Core identity tests include FeatureId without cross-domain conversions.

Compilation now resolves the author entries on the Owner thread, before starting finite work.
CompileState owns its list of stable borrows and borrows the admitted Request and DeviceSession;
it no longer contains an Impl pointer or accesses the mutable feature directory on the worker.
Owner adoption receives Impl only when the completion event is drained. Existing request ownership,
host scheduler, Graph analysis, Pipeline algorithms and last-good publication are unchanged.

Still open: FeatureUse requests must migrate from names/encoded configuration to ID/ConfigValue;
the remaining old registration API, Handler ownership, completion categories, typed Reply and dynamic
installation transaction must converge before the joint N2/N3 gate. The present ListFeatures path
does not imply those other controls or dynamic installation are complete.

Implementation checks: full all -j 4 -- -k 0 passed, second all no-work, 105/105 CTest passed in
182.73 seconds. No latency claim is derived from this suite duration. Evidence is under
E:/RenderV2Rebuild/V12/evidence/N2/identities and identities-{build,no-work,test}.log at its V12 root.
Protection checker passed Legacy 719 and three user files. An exact-commit affected ASan checkpoint
is required before changing the dependent operation ownership; final N2/N3 V remains pending.
