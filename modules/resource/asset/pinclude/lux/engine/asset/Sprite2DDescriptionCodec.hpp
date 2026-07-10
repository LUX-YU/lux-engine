#pragma once
/**
 * @file Sprite2DDescriptionCodec.hpp (private)
 * @brief Compact binary encoder/decoder for `lux::rdesc::SpriteAtlas` and
 *        `lux::rdesc::SpriteAnimClip`. Used internally by Sprite2DSerDeser.
 *
 * Wire format (little-endian, no implicit padding), shared framing:
 *
 *   u32  magic           ('LSAS' atlas / 'LSCS' clip)
 *   u32  endian_tag      = 0x01020304
 *   u32  schema_version  = 1
 *   ...type body...
 *   u32  trailer_magic   ('LSAE' / 'LSCE')
 *
 * Atlas body:
 *   Str  name                       (u32 len + bytes)
 *   u8[16] texture_uuid
 *   u32  frame_count
 *     Frame[frame_count]: Str name, f32[4] uv_rect, f32[2] pivot
 *
 * Clip body:
 *   Str  name
 *   u8[16] atlas_uuid
 *   u8   loop, u8[3] reserved
 *   u32  frame_count
 *     Frame[frame_count]: u32 frame_index, f32 duration
 *   u32  event_count
 *     Event[event_count]: u32 frame_index, u32 event_id
 *
 * Bounds protection (decode fails with error string on violation):
 *   kMaxS2StringLen  = 64 KiB
 *   kMaxS2FrameCount = 65535
 *   kMaxS2EventCount = 65535
 * Version mismatch = explicit failure (no migration framework for v1).
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <lux/engine/description/Sprite2D.hpp>
#include <lux/engine/resource/visibility.h>

namespace lux::asset::detail
{
    inline constexpr std::uint32_t kSaDescMagic     = 0x5341534Cu; // 'LSAS' (LE)
    inline constexpr std::uint32_t kSaDescTrailer   = 0x4541534Cu; // 'LSAE'
    inline constexpr std::uint32_t kScDescMagic     = 0x5343534Cu; // 'LSCS'
    inline constexpr std::uint32_t kScDescTrailer   = 0x4543534Cu; // 'LSCE'
    inline constexpr std::uint32_t kS2EndianTag     = 0x01020304u;
    inline constexpr std::uint32_t kS2SchemaVersion = 1u;

    inline constexpr std::uint32_t kMaxS2StringLen  = 64u * 1024u;
    inline constexpr std::uint32_t kMaxS2FrameCount = 65535u;
    inline constexpr std::uint32_t kMaxS2EventCount = 65535u;

    LUX_RESOURCE_PUBLIC std::vector<std::byte>
    encodeSpriteAtlasDescription(const lux::rdesc::SpriteAtlas& atlas);

    LUX_RESOURCE_PUBLIC bool
    decodeSpriteAtlasDescription(std::span<const std::byte> blob,
                                 lux::rdesc::SpriteAtlas&   out,
                                 std::string*               error_out = nullptr) noexcept;

    LUX_RESOURCE_PUBLIC std::vector<std::byte>
    encodeSpriteAnimClipDescription(const lux::rdesc::SpriteAnimClip& clip);

    LUX_RESOURCE_PUBLIC bool
    decodeSpriteAnimClipDescription(std::span<const std::byte>  blob,
                                    lux::rdesc::SpriteAnimClip& out,
                                    std::string*                error_out = nullptr) noexcept;
}
