# Encoded data, immutable payload ownership and cooked Shader contracts

DataSchema<T> describes an explicit serialized format, never a C++ object layout. Stable identity uses lux-cxx SchemaId. Compatibility compares the full canonical name, exact schema/encoding versions, ordered fields, scalar kinds, array counts, enum domains, fixed size and alignment. Static descriptors and their borrowed strings/arrays must have static lifetime.

Encoding.hpp supplies EncodedDataLimits, EncodedDataView (raw span borrow), RetainableEncodedDataView (borrow of existing SharedBytes owners), EncodableData and explicit encodeData/decodeData entry points. Each specialization provides encodedSize/encode/decode; there is no memcpy fallback. Encoding remains little-endian, canonical scalar/boolean, padding-free and bounded. Renaming Wire did not change byte order, widths, range/alignment validation or golden bytes. Stable error ID strings remain unchanged.

BlobRef addresses a packet range; ExternalDataRef addresses an explicitly supplied attachment owner. Every index/range is checked before a subspan can clamp anything. resolveBlob/resolveExternal retain an existing owner. Views never extend lifetime themselves. encodeData returns a SharedBytes-owned packet but does not retain the separately supplied attachment collection. Callers must retain those owners until decoding ends. Limits are explicit, not a global upload budget.

Payload.hpp includes SharedBytes and RenderError only. RenderPayload retains the complete backing charge and a source-code pin; its byte owner is destroyed before that pin. Copying just its SharedBytes is insufficient to retain a foreign module's deleter. Source buffers remain immutable/stable during use. Typed IPC Command transport moves the actual Payload object; it does not encode it through this API.

ShaderInterface.hpp defines the currently supported Shader descriptor/push-constant ABI, independently of config serialization. SpirvArtifact in Shader.hpp owns cooked SPIR-V arrays and that interface, without Vulkan handles. These contracts do not define business resource authority or scope, and do not introduce a Shader compiler/parser.

Current format consumers are Feature author configuration, registered Storage configuration metadata and concrete Attach binding arguments. Session-local resource IDs in binding bytes are not persistent cooked assets. G2 golden/negative/ownership tests preserve their historical names and assertions. Encoding is cold, with real backing allocations; no full-request zero-allocation claim is made.
