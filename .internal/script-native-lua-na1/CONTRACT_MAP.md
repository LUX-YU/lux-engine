# NA1 contract mapping (implementation in progress)

One core mount owns one identity and ScriptBehavior. NativeLuaTaskBackend owns only composition and child resources.
ScriptInstances/Execution retain permission, retirement, core C/A, Ready, source cancellation and stable-point order.
CppStatic owns native frames and task outcome. Lua owns the main thread, self, prototype and synchronous roots.

The companion receives capability/event arrays reordered to its real artifact declarations, after subset validation.
Both child objects use the same core identity and authority. Original Lua lease stays with core; the companion lease
stays with the composition instance until native child destruction. Begin/End route only through core's original Lua methods.

callStep is a short invocation, not a wait: validate instance/publication, capture original qualification, protect all
Lua operations, restore stack/execution context, recheck original qualification, publish only valid scalar/void output.
No qualification is retained over co_await. Engine await rejects missing resumable context before source admission.
Explicit fail suspends with FAILED and no A/Ready; adapters destroy once and preserve the status.

Cleanup: core revokes -> protected invocation/resume exits -> core cancels/destroys tasks -> EndPlay (when qualified)
-> released external prepared methods -> native child -> invalidated/released sync steps -> Lua child -> companion lease.
The Lua active-continuation count alone is not the composition lifetime authority.

Contracts T01-T54 start NOT_RUN; old tests are not new qualification evidence. Actual mappings will replace this draft.
