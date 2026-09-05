#pragma once
#include <lux/engine/serialization/SerializationError.hpp>
#include <lux/cxx/compile_time/expected.hpp>
/**
 * @file AnimationClipDescriptionCodec.hpp (private)
 * @brief Compact binary encoder/decoder for `lux::rdesc::AnimationClip`.
 *
 * Used internally by AnimationClipSerDeser. NOT part of any public API.
 *
 * Wire format (little-endian, no implicit padding):
 *
 *   u32  magic           = 'LACS' (0x5343414Cu)
 *   u32  endian_tag      = 0x01020304
 *   u32  schema_version  (currently 2 — unorm16 rotation quantization;
 *                          v1 readers fail the version check)
 *   Str  name
 *   f32  duration        (seconds)
 *   u8   loop            (0 = clamp, 1 = wrap)
 *   u8[3] reserved       (alignment / future flags)
 *   u32  track_count
 *     BoneTrack[track_count]:
 *       i32  bone_index
 *       u32  n_t                                (translation key count)
 *       f32[n_t]  times_t                       (seconds, monotonic)
 *       f32[n_t][3]  translations               (Vector3f)
 *       u32  n_r                                (rotation key count)
 *       f32[n_r]  times_r
 *       u16[n_r][4]  rotations                  unorm16 components.
 *                                                Encoder writes
 *                                                  q16 = round((q + 1) * 0.5 * 65535)
 *                                                Decoder undoes
 *                                                  q   = q16 * (2 / 65535) - 1
 *                                                then renormalizes. Worst-case
 *                                                per-component error ~1.5e-5.
 *                                                Halves rotation-key disk size
 *                                                (16 B → 8 B).
 *       u32  n_s                                (scale key count)
 *       f32[n_s]  times_s
 *       f32[n_s][3]  scales                     (Vector3f)
 *   u32  trailer_magic   = 'LACE' (0x4543414Cu)
 *
 * Encoder optimisation (transparent on the wire): constant-track folding.
 * Tracks whose translations / rotations / scales are all equal within a tiny
 * epsilon are collapsed to a single key on encode. The decoder is unchanged
 * (a 1-key track is already a valid representation; the runtime sampler
 * clamps any t to that key's value). Typical Mixamo source clips contain
 * many "scale = identity forever" tracks — folding them saves ~all-but-one
 * keys per such track.
 *
 * Bounds protection (decode fails with error string on violation):
 *   kMaxAcStringLen   = 64 KiB
 *   kMaxAcTrackCount  = 65535      (≥ any sane bone count)
 *   kMaxAcKeyCount    = 1 << 20    (~1M keys / track → ~ generous; a 1-hour
 *                                    clip @ 60Hz = 216k keys, well under)
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <lux/engine/description/AnimationClip.hpp>
#include <lux/engine/resource/asset/visibility.h>

namespace lux::asset::detail
{
    inline constexpr std::uint32_t kAcDescMagic       = 0x5343414Cu; // 'LACS' (LE)
    inline constexpr std::uint32_t kAcDescTrailer     = 0x4543414Cu; // 'LACE'
    inline constexpr std::uint32_t kAcEndianTag       = 0x01020304u;
    inline constexpr std::uint32_t kAcSchemaVersion   = 2u;          // unorm16 rotations

    inline constexpr std::uint32_t kMaxAcStringLen    = 64u * 1024u;
    inline constexpr std::uint32_t kMaxAcTrackCount   = 65535u;
    inline constexpr std::uint32_t kMaxAcKeyCount     = 1u << 20;

    /// Encode an AnimationClip into a compact binary blob. Never fails
    /// (other than std::bad_alloc).
    /// LUX_ASSET_PUBLIC: see the matching rationale in
    /// SkeletonDescriptionCodec.hpp — pinclude not installed.
    using AnimationClipDescriptionEncodeResult =
        lux::cxx::expected<std::vector<std::byte>, lux::serialization::SerializationFailure>;
    LUX_ASSET_PUBLIC AnimationClipDescriptionEncodeResult
    encodeAnimationClipDescription(const lux::rdesc::AnimationClip& clip);

    /// Decode an AnimationClip. Returns false and (optionally) writes a
    /// human-readable error message into *error_out on any malformed input.
    LUX_ASSET_PUBLIC bool
    decodeAnimationClipDescription(std::span<const std::byte>      blob,
                                   lux::rdesc::AnimationClip&       out,
                                   std::string*                     error_out = nullptr) noexcept;
}
