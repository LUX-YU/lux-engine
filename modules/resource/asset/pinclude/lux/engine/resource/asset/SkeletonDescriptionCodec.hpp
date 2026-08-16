#pragma once
/**
 * @file SkeletonDescriptionCodec.hpp (private)
 * @brief Compact binary encoder/decoder for `lux::rdesc::Skeleton`.
 *
 * Used internally by SkeletonSerDeser. NOT part of any public API.
 *
 * Wire format (little-endian, no implicit padding):
 *
 *   u32  magic                 = 'LSKS' (0x534B534Cu)
 *   u32  endian_tag            = 0x01020304
 *   u32  schema_version        (currently 2)
 *   u32  bone_count
 *     Bone[bone_count]:
 *       Str   name             (Str = u32 byte_len + UTF-8 bytes)
 *       i32   parent_index     (-1 for root, else < bone_count)
 *       f32[12]  bind_local    (Affine3f, 3x4 column-major; row 3 = [0,0,0,1])
 *       f32[12]  inv_bind_world (same layout)
 *   f32[12]  global_transform  (Affine3f; armature prefix for root bones, v2+)
 *   u32  trailer_magic         = 'LSKE' (0x454B534Cu)
 *
 * Affine3f storage: Eigen::Affine3f internally stores 4x4 but the last row
 * is always [0,0,0,1] for an affine map. The codec elides that row to save
 * 16 bytes per matrix (32 bytes per bone) at zero information loss.
 *
 * Bounds protection (decode fails with error string on violation):
 *   kMaxStringLen  = 64 KiB
 *   kMaxBoneCount  = 65535      (well above any sane skeleton; ~ 4000 typical
 *                                 humanoid + accessories ceiling)
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <lux/engine/description/Skeleton.hpp>
#include <lux/engine/resource/asset/codecs/visibility.h>

namespace lux::asset::detail
{
    inline constexpr std::uint32_t kSkeletonDescMagic    = 0x534B534Cu; // 'LSKS' (LE)
    inline constexpr std::uint32_t kSkeletonDescTrailer  = 0x454B534Cu; // 'LSKE'
    inline constexpr std::uint32_t kSkeletonEndianTag    = 0x01020304u;
    inline constexpr std::uint32_t kSkeletonSchemaVersion = 2u; // v2: + Skeleton::global_transform

    inline constexpr std::uint32_t kMaxSkelStringLen     = 64u * 1024u;
    inline constexpr std::uint32_t kMaxSkelBoneCount     = 65535u;

    /// Encode a Skeleton into a compact binary blob. Never fails (other
    /// than std::bad_alloc).
    /// Marked LUX_RESOURCE_PUBLIC so the asset module's own test target can
    /// link against it through the pinclude path; pinclude headers are not
    /// installed, so external consumers still cannot see this declaration.
    LUX_ASSET_CODECS_PUBLIC std::vector<std::byte>
    encodeSkeletonDescription(const lux::rdesc::Skeleton& skel);

    /// Decode a Skeleton. Returns false and (optionally) writes a
    /// human-readable error message into *error_out on any malformed input.
    LUX_ASSET_CODECS_PUBLIC bool
    decodeSkeletonDescription(std::span<const std::byte> blob,
                              lux::rdesc::Skeleton&       out,
                              std::string*                error_out = nullptr) noexcept;
}
