#pragma once
/**
 * @file ThumbnailSet.hpp
 * @brief On-disk container for an asset's pre-rendered thumbnails.
 *
 * A `ThumbnailSet` is a small bag of images at several resolutions (the editor
 * renders ~3 sizes so different UI surfaces — list row, grid tile, big preview
 * — each get crisp pixels without rescaling at draw time). The whole set is
 * serialized into a SINGLE generic asset payload (see `Asset.hpp`'s `Payload`),
 * tagged with `kThumbnailPayloadTag`.
 *
 * Layering note: thumbnails are an EDITOR concept, so the tag + container live
 * here, NOT in the engine-generic asset layer. The asset layer only knows
 * "opaque tagged binary blob"; it neither produces nor interprets thumbnails.
 * External apps that don't link the editor simply see an unknown payload tag
 * and skip it.
 *
 * Blob layout (little-endian, matches the engine's memcpy-of-POD convention):
 *
 *     ThumbnailSetHeader { magic, version, count, reserved }
 *     repeat count times:
 *         ThumbnailImageHeader { width, height, encoding, byte_size }
 *         byte_size bytes of image data (encoding-specific)
 */

#include <lux/engine/resource/asset/Asset.hpp>   // lux::asset::payload_tag_t, Payload

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lux::editor
{
    /// Magic payload tag identifying a serialized `ThumbnailSet`. ASCII
    /// "LUX_THMB" — distinctive enough that a foreign payload won't collide.
    inline constexpr lux::asset::payload_tag_t kThumbnailPayloadTag =
        0x4C55585F54484D42ULL; // 'L''U''X''_''T''H''M''B'

    /// How a single thumbnail image's bytes are encoded.
    enum class EThumbnailEncoding : std::uint32_t
    {
        RAW_BGRA8 = 0, ///< tightly-packed BGRA8, row-major, top-left origin
        PNG       = 1, ///< PNG container (RGBA8) — compact, the default on disk
    };

    /// One thumbnail at one resolution.
    struct ThumbnailImage
    {
        std::uint32_t           width{0};
        std::uint32_t           height{0};
        EThumbnailEncoding      encoding{EThumbnailEncoding::PNG};
        std::vector<std::byte>  bytes;
    };

    /// A bag of thumbnails at several resolutions for one asset.
    struct ThumbnailSet
    {
        std::vector<ThumbnailImage> images;

        [[nodiscard]] bool empty() const noexcept { return images.empty(); }

        /// Pick the smallest image whose width AND height are >= the request,
        /// else the largest available (so callers always get *something*).
        [[nodiscard]] const ThumbnailImage* best(std::uint32_t w, std::uint32_t h) const noexcept;
    };

    // ── (De)serialization to / from a payload blob ──────────────────────

    /// Serialize @p set into a flat byte blob suitable for a `Payload::data`.
    [[nodiscard]] std::vector<std::byte> encodeThumbnailSet(const ThumbnailSet& set);

    /// Parse a payload blob back into a `ThumbnailSet`. Returns nullopt if the
    /// magic/version don't match or the blob is truncated.
    [[nodiscard]] std::optional<ThumbnailSet> decodeThumbnailSet(std::span<const std::byte> blob);

    // ── Convenience: read / write the thumbnail payload on a LuxAsset ────

    /// Read + decode the thumbnail payload of @p asset, if present.
    [[nodiscard]] std::optional<ThumbnailSet> readThumbnailSet(const lux::asset::LuxAsset& asset);

    /// Encode @p set and store it as @p asset's thumbnail payload (in memory;
    /// the caller re-exports the `.luxasset` to persist it).
    void writeThumbnailSet(lux::asset::LuxAsset& asset, const ThumbnailSet& set);

} // namespace lux::editor
