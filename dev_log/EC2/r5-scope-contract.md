# R5 scope implementation contract (working design, not execution evidence)

Original ScriptInstances::Construction allocates the real instance identity before backend creation.
Add optional prepare_instance to the existing publication/prepared capability. A move-only
ScriptApiInstanceBinding returned by preparation owns one native scope; the mount owns these bindings.
Backend receives only the substituted context. Revoke once at original identity revocation, before
backend endPlay/destroy; clear scopes after backend destruction and before releasing artifact code.
Rollback uses the same retirement path. No caller ID from script, no TLS, no parallel ScriptSystem.

ScriptAssetAccess owns fixed read capability, bounded instance admission and one original TaskScope.
Each native ScriptAssetScope owns bounded result slots and a stop source; accepted completions retain
scope data, not VM/Registry. Revocation marks stopping, invalidates result domain and requests only its
own accepted work cancellation. Access remains the execution owner until accepted tasks settle.
Provider shutdown marks all scopes revoked and joins its TaskScope at the host destruction boundary.
No scope destruction waits inside ScriptSystem retirement, and no completion resumes VM directly.

Results use existing ScopeIdSource in one compiled provider, plus SlotMap generation. A handle is an
alias, not an owning reference; release invalidates all aliases. The native typed result stores its
code pin before the asset owner so its deleter runs before last code release. Existing codec/read
algorithm is reused. Capacity counts logical reservation even for physically shared input bytes.

ReadAssetImage now forwards max_bytes via AssetVfs and every real provider. Pak rejects before payload
allocation; metadata index pages retain their independent original bound. This is not a global RSS cap.
Native decoder/retained-result budgets and temporary backend costs remain separate measurements.

Lua will use the original bounded TLuaValueCodec/ScriptAbility/LuaBoundary. Opaque transport requires
one narrow typed userdata value primitive in that existing codec (semantic ID + representation + exact
size, no GC ownership), not an asset-specific VM branch. AssetId retains all 128 bits; large values are
not converted to double. Business failures are fixed outcomes; protocol rejection is the existing start
error. Completion stale releases any reserved result. Scope revocation also covers completion accepted
before coroutine resume.

R5 implementation refinement: scene_script_assets is SHARED, solely to keep one compiled ScopeIdSource across consumer DLLs. Native and optional Lua targets remain separate. Original ingress can return BACKPRESSURE; completed outcomes stay in the original bounded result record and deliverCompletions retries fixed identity batches. No replacement read/decode or executor is introduced. Final ScriptRuntimeSystem wiring is R7. Native tests are development evidence only, not Lua/PLAYER qualification.
