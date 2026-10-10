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

## Generated projections and domain policy

Runtime reflection remains a cold/dynamic capability. New Inspector, component codec/operation and script
dispatch paths should use generated typed operations; a new hot-path dependency on runtime reflection needs
an explicit domain/performance reason. Existing FlowForge RefType, RuntimeObject and ReflectionRegistry
consumers remain supported; this is not a runtime-reflection removal programme.

`meta.projection_compatibility` runs one real lux-cxx parser job with the unmodified Engine static-info and
lux-cxx serialization templates. The representative records include scalar, nested record, string, enum,
vector and array members, private fields, independent skip annotations and a serialized-name override.
The test compares generated field order, pointer identity and actual typed access, not handwritten stand-ins.

These projections share declaration parsing, but are not interchangeable:

| Contract | Engine `TTypeStaticInfo` | lux-cxx `ser::meta_info` |
|---|---|---|
| Public field order/access | Declaration order; compile-time member pointer | Declaration order; typed member pointer |
| `skip_static` | Omits the descriptor | Does not change serialization participation |
| `serializable, skip` | Does not change static field participation | Retains descriptor with `options.skip` |
| Name override | Original member name | Serialized key override |
| Private fields | Omitted | Omitted |
| Enum | Field retains enum type; no enum-name table in this projection | Separate generated `enum_meta` table |
| Containers/nesting | Retains exact member type | Retains exact member type; traversal belongs to its backend |

The policy fixture deliberately yields seven effective fields on each side with different membership.
Equal counts cannot prove schema equivalence. Serialization participation must not determine Inspector
visibility, and the two annotation families must not be silently merged.

Engine BinaryWriter/Reader, entity remapping, world archive limits and semantic archive rules remain the
authoritative persistence contract. Reusing a parser or member descriptor does not authorize replacing
that contract with a JSON/XML backend. Additional editor/component projections belong to their domain.

The existing MetaModuleRegistrar compatibility mechanism remains for its current consumers. New systems
must not rely on hidden static registration; dynamic extensions supply explicit registration and retain
their code through the existing definition/value owners. This work does not implement plugin unloading.
