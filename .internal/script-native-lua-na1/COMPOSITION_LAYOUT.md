# NA1 composition layout (implementation in progress)

A fixed-capacity facade Instance contains original identity, publication epoch, Lua/native child handles, companion
artifact lease, stable step entries, reordered capability/event spans and fixed prepared routing storage. It does not
own a second task table or scheduler. All published addresses are reserved before child construction/publication.
A prepared method routes directly to the selected child entry; only release metadata remains in the facade.
Each native task is the existing CppStatic CoroutineContinuation plus compiler frame and optional owned argument block.
The frame keeps only cross-wait business values. Main-thread Lua execution state is borrowed per call, never per task.

Record separately: actual sizeof/stride, reserved payload and metadata, active/high-water frames, facade and routing,
Lua object/live/idle/root resources, core C/A/Ready/source/transport, and leases. Shared backing is counted once.
Lua task capacity may be zero in sync-only compositions. B_KEEP_LUA_RESERVE is a resource-only comparison.
