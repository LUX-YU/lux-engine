#pragma once
#include <cstdint>
#include <vector>

namespace lux::ui
{
    struct GlyphRange final
    {
        char32_t first{}, last{}; // Inclusive; sorted, non-overlapping BMP ranges, excluding surrogates.
    };

    // Cold input only. create() copies this data; failure leaves the caller's value unchanged.
    // Root retains its own bytes and ranges until AFTER its font atlas/context are destroyed.
    struct FontSource final
    {
        std::vector<std::uint8_t> bytes;
        std::vector<GlyphRange> ranges;
        std::uint32_t face{};
        float size_pixels{18}; // UI content units before RootConfig::scale; independent from framebuffer scale.
    };

    enum class EInitError : std::uint8_t
    {
        ALLOCATION_FAILURE,
        INVALID_FONT_DATA,
        INVALID_FONT_FACE,
        INVALID_GLYPH_RANGE,
        INVALID_FONT_SIZE,
        FONT_LIMIT,
        ATLAS_FAILURE,
        ATLAS_LIMIT,
        WRONG_THREAD,
        INVALID_DISPATCHER,
        INVALID_INPUT_CAPACITY,
        INVALID_SCALE
    };
} // namespace lux::ui
