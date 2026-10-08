# Lua VM ownership

Use `ScriptEngine::create(configuration)`. A successful, fixed-address Engine always has a complete VM.
Invalid configuration, VM creation and bootstrap rejection have distinct `ELuaEngineError` values.
The native VM candidate owns its Lua state immediately; allocator storage remains at one address from
`lua_newstate` through `lua_close`. VM finalizers run before error-handler captures and allocator storage
are destroyed. No public `init` or construction-validity query is required.

`ScriptRef` remains a borrowed reference into that Engine and must be destroyed before its Engine.
Returning or storing a ScriptRef does not prolong the VM lifetime. This change does not introduce
another script executor or alter LuaBoundary's protected execution protocol.

The construction test compiles the actual private owner with native-call fault injection. It verifies
typed VM/bootstrapping rejection and zero remaining Lua allocations after each VM is closed. The public
installed consumer independently verifies configuration rejection, script execution and finalizer order.
