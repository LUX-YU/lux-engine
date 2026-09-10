#pragma once
#include <lux/engine/ui/UiFontSource.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <cmath>
#include <span>

namespace lux::ui::detail
{
    // Validate the SFNT envelope before invoking the pinned trusted-font rasterizer.
    // This is not a sanitizer for arbitrary hostile font programs.
    inline lux::cxx::expected<void, EUiInitError> validateFont(const UiFontSource &source) noexcept
    {
        auto fail = [](EUiInitError error) { return lux::cxx::unexpected(error); };
        if (source.bytes.empty())
            return fail(EUiInitError::INVALID_FONT_DATA);
        if (source.bytes.size() > 32U * 1024U * 1024U)
            return fail(EUiInitError::FONT_LIMIT);
        if (!std::isfinite(source.size_pixels) || source.size_pixels < 6 || source.size_pixels > 64)
            return fail(EUiInitError::INVALID_FONT_SIZE);
        if (source.ranges.empty() || source.ranges.size() > 64)
            return fail(EUiInitError::INVALID_GLYPH_RANGE);
        char32_t previous{};
        std::uint32_t glyphs{};
        for (const auto range : source.ranges)
        {
            const bool overlaps_surrogate = range.first <= 0xDFFF && range.last >= 0xD800;
            const bool invalid = range.first == 0 || range.first <= previous || range.first > range.last ||
                                 range.last > 0xFFFF || overlaps_surrogate;
            if (invalid)
                return fail(EUiInitError::INVALID_GLYPH_RANGE);
            glyphs += static_cast<std::uint32_t>(range.last - range.first + 1);
            previous = range.last;
        }
        if (glyphs > 32768)
            return fail(EUiInitError::FONT_LIMIT);
        const auto data = std::span{source.bytes};
        const auto fits = [&](std::size_t offset, std::size_t length) {
            return offset <= data.size() && length <= data.size() - offset;
        };
        const auto u16 = [&](std::size_t offset) { return (std::uint32_t(data[offset]) << 8) | data[offset + 1]; };
        const auto u32 = [&](std::size_t offset) { return (u16(offset) << 16) | u16(offset + 2); };
        if (!fits(0, 12))
            return fail(EUiInitError::INVALID_FONT_DATA);
        std::size_t face_offset{};
        if (u32(0) == 0x74746366U) // TTC: table offsets remain relative to the entire collection.
        {
            const auto count = u32(8);
            if (!count || count > 256 || !fits(12, std::size_t(count) * 4))
                return fail(EUiInitError::INVALID_FONT_DATA);
            if (source.face >= count)
                return fail(EUiInitError::INVALID_FONT_FACE);
            face_offset = u32(12 + std::size_t(source.face) * 4);
        }
        else if (source.face != 0)
            return fail(EUiInitError::INVALID_FONT_FACE);
        if (!fits(face_offset, 12))
            return fail(EUiInitError::INVALID_FONT_DATA);
        // This cold path supports TrueType outlines. CFF/WOFF require a separate, unapproved codec.
        const auto signature = u32(face_offset);
        if (signature != 0x00010000U && signature != 0x74727565U)
            return fail(EUiInitError::INVALID_FONT_DATA);
        const auto count = u16(face_offset + 4);
        if (!count || count > 256 || !fits(face_offset + 12, std::size_t(count) * 16))
            return fail(EUiInitError::INVALID_FONT_DATA);
        unsigned required{};
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto entry = face_offset + 12 + i * 16;
            const auto tag = u32(entry), offset = u32(entry + 8), length = u32(entry + 12);
            if (!fits(offset, length))
                return fail(EUiInitError::INVALID_FONT_DATA);
            switch (tag)
            {
            case 0x636D6170:
                if (length >= 4)
                    required |= 1;
                break; // cmap
            case 0x68656164:
                if (length >= 54)
                    required |= 2;
                break; // head
            case 0x68686561:
                if (length >= 36)
                    required |= 4;
                break; // hhea
            case 0x686D7478:
                if (length >= 4)
                    required |= 8;
                break; // hmtx
            case 0x6D617870:
                if (length >= 6)
                    required |= 16;
                break; // maxp
            case 0x676C7966:
                if (length)
                    required |= 32;
                break; // glyf
            case 0x6C6F6361:
                if (length)
                    required |= 64;
                break; // loca
            }
        }
        if (required != 127)
            return fail(EUiInitError::INVALID_FONT_DATA);
        return {};
    }
} // namespace lux::ui::detail
