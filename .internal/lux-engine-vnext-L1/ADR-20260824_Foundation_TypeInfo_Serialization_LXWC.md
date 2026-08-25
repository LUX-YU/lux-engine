# ADR: Foundation TypeInfo, exact binary serialization and LXWC columns

- Date: 2026-08-24
- Status: Foundation decisions retained; LXWC v1 portions superseded by
  `ADR-20260825_World_Data_Path_LXWC_v2.md`
- Supersedes: the codec/reflection and LXWS persistence portions of
  `ADR-20260824_L1_v2_Mutation_Schedule_Hierarchy.md`

## Decision

Engine build-time metadata is a set of projections over one parsed AST.
`lux_add_codegen_job`, `lux_codegen_add_projection`, and
`lux_target_add_codegen` are the only generator CMake API. Physical paths are
parse inputs; logical paths determine collision-safe output paths. All
projections render before any output is published and content-identical output
keeps its timestamp. The removed `add_meta` and `target_add_meta` APIs have no
compatibility aliases.

`lux::meta::TypeStaticInfo<T>` is a header-only, declaration-order tuple of
member pointers. It contains no offsets, runtime lookup, registrar, or
host-generated ABI facts. Runtime TypeInfo is a separate projection. Lua,
Render communication, Render pass-parameter, ECS schema, and ECS persistence
projections are owned by their consumer modules rather than the core runtime
reflection template.

`lux::serialization` in `modules/core/serialization` is Engine's exact,
portable binary contract. It is independent of `lux::cxx::ser`, which remains
the JSON/XML/YAML/configuration system. Binary dispatch is compile-time:
explicit `Serializer<T>`, built-ins/STL/external specializations,
`TypeStaticInfo<T>`, then a compile error. The binary path has no runtime
reflection fallback, field-name lookup, or runtime type switch. Integral,
floating-point, container-length, Eigen, and UUID representations are explicitly
little-endian and bounded by `SerializationLimits`.

ECS persistence erases type exactly once at the complete Component-column
boundary. `ComponentPersistenceBinding` contains only a schema pointer and the
automatically instantiated `encodeColumn<T>/decodeColumn<T>` thunks. A thunk
acquires typed storage once and statically serializes every selected value.
Custom Component callbacks, `ComponentCodec`, runtime `RefClass/RefField`,
global registries, and static registrars are forbidden.

Bindings are collected in immutable, module-scoped
`ComponentPersistenceContribution` groups. Each group pins the code that owns
its thunk. `WorldSectionWriter` and `WorldSectionReader` build a sorted,
call-scoped lookup and hold contribution pins for the call. Snapshot remains a
schema/clone operation consumer and does not depend on persistence bindings.

`EcsBinaryWriter/Reader` compose the core binary reader/writer and add only
local `Entity` ordinal semantics. Core serialization does not know ECS.
`PersistentEntityRef` is serialized as a stable value. Payload relocation
tables and property tables are retired because Entity references are already
encoded in their relocatable representation.

## LXWC v1 (historical, retired by the World data-path decision)

The new wire magic is `LXWC`, version 1, fixed little-endian. `LXWS` is
rejected as `INVALID_MAGIC`; there is no reader shim or migration callback.

A section contains schema/version entries, PersistentEntityId-sorted entities,
archetype signatures, and Component columns. Each column contains entity
ordinals and one contiguous payload. Equal-width rows use stride form; other
rows use `N+1` offsets. Unknown-schema columns survive image decode unchanged.
Materialization requires an exact schema ID/version/C++ type binding.

The bridge does not know section/archetype layout. It only sees typed World
storage, the selected entity span, and an ECS binary reader/writer. Dynamic
dispatch count is therefore the number of encoded/decoded columns, never the
number of entities or fields.

`ComponentEncodePort::write(type, bytes)` no longer exists. For every custom
`Serializer<T>`, bytes written to a binary writer are serializer-owned portable
representation; a semantic wire type never converts arbitrary host object bytes
to little-endian.

## Freeze line

This foundation change invalidates the previous L1 v2.1 persistence freeze
evidence even though World/Journal/Schedule/Hierarchy hardening is retained.
The maximum state after the full generator, TypeInfo, serialization, LXWC,
platform, install, and performance matrix is:

```text
Foundation Qualified
L1 Architecture Accepted
Correctness Hardened
Performance Contract Passed
Public API Freeze Candidate
Independent Re-audit Required
Domain Migration Blocked
```

The TypeInfo and exact binary serialization decisions remain active. The
column writer/image and freeze line in this document are superseded by the
LXWC v2 runtime-load ADR.
