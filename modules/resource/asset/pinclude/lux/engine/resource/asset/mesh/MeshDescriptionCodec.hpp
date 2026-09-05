#pragma once
#include <lux/engine/serialization/SerializationError.hpp>
#include <lux/cxx/compile_time/expected.hpp>
/**
 * @file MeshDescriptionCodec.hpp (private)
 * @brief Compact binary encoder/decoder for `lux::rdesc::Mesh`.
 *
 * Used internally by MeshSerDeser. NOT part of any public API.
 *
 * Wire format (little-endian, no implicit padding):
 *
 *   u32  magic                 = 'LMSH' (0x48534D4Cu)
 *   u32  endian_tag            = 0x01020304
 *   u32  schema_version        (currently 2)
 *   u32  vertex_count
 *   u32  index_count
 *   u32  has_bounds            (0 or 1)
 *   Vertex[vertex_count]       raw POD bytes (sizeof(Vertex) each)
 *   u32[index_count]           raw POD bytes
 *   if has_bounds: f32[6]      AABB (min.xyz, max.xyz)
 *   --- schema v2+ (decode of v1 leaves lods empty; no re-bake required) ---
 *   u32  lod_count             (LOD1..N; 0 == single-LOD mesh)
 *   per LOD: u32 index_count; f32 error; u32[index_count] raw indices
 *   u32  trailer_magic         = 'LMSE' (0x45534D4Cu)
 *
 * `Vertex` is a trivially-copyable POD (4× Vector3f + Vector2f + Bone{int[4],
 * float[4]} = 88 bytes, no padding), so the large vertex / index arrays are
 * bulk-copied rather than written element-wise. A `static_assert` in the .cpp
 * pins `sizeof(Vertex)`; changing the struct forces a schema-version bump. The
 * endian tag guards the little-endian / raw-POD assumption (decode fails on
 * mismatch).
 *
 * Bounds protection (decode fails with an error string on violation):
 *   kMaxMeshVertexCount / kMaxMeshIndexCount = 16,777,216 (1 << 24)
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/resource/asset/visibility.h>

namespace lux::asset::detail
{
    inline constexpr std::uint32_t kMeshDescMagic     = 0x48534D4Cu; // 'LMSH' (LE)
    inline constexpr std::uint32_t kMeshDescTrailer   = 0x45534D4Cu; // 'LMSE'
    inline constexpr std::uint32_t kMeshEndianTag     = 0x01020304u;
    inline constexpr std::uint32_t kMeshSchemaVersion = 2u; // v2: appended LOD chain (decode tolerant of v1)

    inline constexpr std::uint32_t kMaxMeshVertexCount = 1u << 24; // ~16.7M, defensive
    inline constexpr std::uint32_t kMaxMeshIndexCount  = 1u << 24;
    inline constexpr std::uint32_t kMaxMeshLodCount    = 8u;          // defensive cap on LOD levels

    /// Encode a Mesh into a compact binary blob. Reports allocation and layout-budget failure. Marked LUX_ASSET_PUBLIC so the asset module's own
    /// test target can link against it through the pinclude path; pinclude
    /// headers are not installed, so external consumers cannot see it.
    using MeshDescriptionEncodeResult =
        lux::cxx::expected<std::vector<std::byte>, lux::serialization::SerializationFailure>;
    LUX_ASSET_PUBLIC MeshDescriptionEncodeResult
    encodeMeshDescription(const lux::rdesc::Mesh& mesh);

    /// Decode a Mesh. Returns false and (optionally) writes a human-readable
    /// error message into *error_out on any malformed input.
    LUX_ASSET_PUBLIC bool
    decodeMeshDescription(std::span<const std::byte> blob,
                          lux::rdesc::Mesh&          out,
                          std::string*               error_out = nullptr) noexcept;
}
