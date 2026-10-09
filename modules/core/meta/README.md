# Runtime reflection values

`RuntimeObject` is a nullable, movable value owner. Empty is a valid value: cloning it succeeds with empty.
Typed trivial values (including `std::string_view`) use builtin metadata without consulting the runtime registry.
A string view owns its view value, not its characters; the caller retains the referenced character storage.

`defaultOf(const RefType&)` returns a zero-filled trivial value, `INVALID_TYPE` for invalid layout,
or `DEFAULT_UNAVAILABLE` for a type requiring construction. `create` reports unavailable or failed construction.
`clone() const` creates an independent allocation and distinguishes unavailable copying from a foreign copy failure.
It never modifies its source. Ordinary heap exhaustion is fatal; foreign callback exceptions are contained only
at these fallible factory boundaries.

RefType/RefClass metadata must outlive its values and clones. Dynamic registration uses the existing
ReflectionRegistrationDraft code owner: release external library ownership after publication, destroy runtime
values, then destroy the registry. RuntimeObject does not create a second metadata or code ownership registry.
Generated and special-support callbacks operate on the reflected C++ type, never on RefClass itself.
