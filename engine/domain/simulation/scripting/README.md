# Script backend construction

CppStatic and Lua backends publish only complete runtime state. Preparation owns its actual slabs,
bounded storage and VM; a rejected candidate releases those resources before returning the typed error.
The runtime constructors consume the prepared owners without a `valid` or `vm_configured` handshake.
Backend move operations transfer the same state allocation, preserving all callback addresses.

CppStatic preparation retains the generated contracts as borrowed immutable data. Their code and data
must outlive the backend. Invalid pool configuration returns `INVALID_DESCRIPTOR`; ordinary heap OOM
is fatal. The backend's coroutine, instance, association and method state still represent runtime facts.

Lua prepares bounded ability/event pools first, then creates and configures its VM through ScriptEngine.
Value preparation and root-table creation finish before the backend Impl is constructed. A zero-capacity
ability or event pool is a supported empty pool. Capacity failures remain `INVALID_CAPACITY`; rejected
VM configuration or extension preparation remains `VM_CONFIGURATION_FAILURE` at the backend boundary.

The native and installed construction regressions exercise repeated partial preparation failure,
actual Lua finalization, backend move assignment and the empty-pool case. Existing ScriptSystem/asset
tests retain instance, coroutine, code-owner and cancellation coverage; construction does not replace
those runtime protocols.
