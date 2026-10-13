# Render Data — Draft4 G2

`render_data` describes and validates neutral wire values. It depends only on
`render_core` and lux-cxx core/binary/memory. No routing, Scope, GPU layout, business
data, logging, service, registry, or Native ownership belongs here.

## Schema and codec contract

`DataTypeId` / `DataTypeIdView` are lux-cxx SchemaId aliases, not runtime object IDs.
Equality includes the canonical name as well as its hash. Compatibility additionally
compares both complete versions, layout and all fields, including enum domains.
Minor versions must match exactly. The descriptor's alignment and fixed_size are
wire metadata; they never describe a C++ object representation.

Each `DataSchema<T>` specialization supplies static `describe()` metadata and
noexcept `wireSize`, `encode`, and `decode` functions. There is no default memcpy
codec, including for trivially copyable structs. The public `WireData` concept
requires these explicit operations. Codec implementations are trusted application
code: a concept cannot prove arbitrary C++ callback bodies safe. Their wire output
and input still pass the common structural validator. Examples live only in tests.

Fields are packed in declaration order, without native padding. Supported scalars
are fixed-width integers, canonical IEEE float32/float64 and canonical boolean
bytes. Fixed arrays repeat the same field encoding. Unsigned integer fields can
declare an exact enum domain. UTF8/BYTES scalar kinds are rejected in G2: use an
explicit byte reference; this is not a generic string/serialization framework.

Each inline BlobRef encodes little-endian uint64 offset and size. ExternalDataRef
prepends a uint32 attachment index to that pair. Inline blobs follow the fixed
record in field/array order, with zero padding to the declared relative alignment.
No overlap, unexplained gap, nonzero padding or trailing bytes are accepted. Zero
length ranges are valid if their offset is valid. External references can overlap
because they borrow independently owned immutable backing; the complete range must
exist and will never be silently clamped. No typed pointer dereference is used;
the wire buffer itself may start at an unaligned address.

Schema descriptors are not copied into an on-wire envelope. The caller supplies
the actual descriptor, and typed decode compares it to its static expected schema
before reading data. A future G3 binding must establish this metadata contract;
supplying the expected descriptor regardless of the actual source is not validation.
No packet header, TypeId table or routing protocol is invented in G2.

## Ownership and failure

`OwnedWireBytes` is exactly `cxx::SharedBytes<>`. `WireView` borrows its descriptor,
packet owner and attachment span for one synchronous call. `resolveBlob` and
`resolveExternal` return retained byte owners; decode results that need bytes after
return must store these owners, not spans. `SharedBytes::fromOwner` requires the
source to keep storage stable and immutable. The library cannot validate an
arbitrary caller-provided pointer's relationship to its shared owner.

`encodeWire` returns ownership of the packet only. The caller must separately keep
the attachment collection alive for a later decode/transfer. A future transport
packet will own that collection; G2 does not implement its admission or settlement.
The encoder allocates a vector backing and retains it through SharedBytes without
copying the completed packet. Its control block is also an allocation; no zero-
allocation or real-time encoding claim is made.

WireLimits has no permissive default: zero limits accept no nonempty wire/range.
`max_wire_bytes` bounds the complete packet; `max_reference_bytes` bounds each
referenced range. These are not an aggregate upload admission budget. Size/range
checks precede allocation or access, and typed decode publishes only a complete
value. Codec errors propagate as RenderError; ordinary allocation OOM terminates
through the noexcept boundary. No exception recovery or terminal output is added.

## Verification boundary

Tests cover independent scalar/array and owned-binary schemas, byte goldens,
negative structure/range/codec cases, real owner destruction, installed consumers
and G1 regression. They remain active with NDEBUG. GPU and SyncValidation are N/A.
Feature schemas, code generation, schema migration and G3 transport remain absent.
