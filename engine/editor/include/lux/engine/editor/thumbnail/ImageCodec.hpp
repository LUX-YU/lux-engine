#pragma once
/**
 * @file ImageCodec.hpp
 * @brief Small image helpers for the thumbnail subsystem: PNG encode/decode
 *        (compact on-disk payloads), sRGB-aware downscale (one render → many
 *        sizes), and a BGRA↔RGBA swizzle (GPU readback is BGRA; PNG is RGBA).
 *
 * Implemented over stb (vcpkg). Kept editor-local so the engine-generic asset
 * layer stays free of any image-codec dependency.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lux::editor
{
    struct DecodedImage
    {
        std::uint32_t          width{0};
        std::uint32_t          height{0};
        std::vector<std::byte> rgba8; ///< tightly packed RGBA8, row-major
    };

    /// Encode tightly-packed RGBA8 pixels → PNG bytes. Empty on failure.
    [[nodiscard]] std::vector<std::byte>
    encodePngRgba8(const std::byte* rgba, std::uint32_t w, std::uint32_t h);

    /// Decode PNG (or any stb-supported image) → RGBA8. nullopt on failure.
    [[nodiscard]] std::optional<DecodedImage>
    decodePngRgba8(std::span<const std::byte> data);

    /// sRGB-aware box downscale RGBA8 → RGBA8. Empty on failure.
    [[nodiscard]] std::vector<std::byte>
    downscaleRgba8(const std::byte* src, std::uint32_t sw, std::uint32_t sh,
                   std::uint32_t dw, std::uint32_t dh);

    /// In-place swap of the B and R channels (BGRA8 ↔ RGBA8), @p pixel_count pixels.
    void swizzleBgraRgba(std::byte* pixels, std::size_t pixel_count);

} // namespace lux::editor
