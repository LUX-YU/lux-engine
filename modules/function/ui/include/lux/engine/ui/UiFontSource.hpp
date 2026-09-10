#pragma once
#include <cstdint>
#include <vector>

namespace lux::ui
{
    struct UiGlyphRange final
    {
        char32_t first{}, last{}; // Inclusive; sorted, non-overlapping BMP ranges, excluding surrogates.
    };

    // Cold input only. create() copies this data; failure leaves the caller's value unchanged.
    // The session retains its own bytes and ranges until AFTER its font atlas/context are destroyed.
    struct UiFontSource final
    {
        std::vector<std::uint8_t> bytes;
        std::vector<UiGlyphRange> ranges;
        std::uint32_t face{};
        float size_pixels{18}; // UI logical pixels. No DPI multiplication is performed during atlas construction.
    };

    enum class EUiInitError : std::uint8_t
    {
        ALLOCATION_FAILURE,
        INVALID_FONT_DATA,
        INVALID_FONT_FACE,
        INVALID_GLYPH_RANGE,
        INVALID_FONT_SIZE,
        FONT_LIMIT,
        ATLAS_FAILURE,
        ATLAS_LIMIT,
        WRONG_THREAD
    };
}
