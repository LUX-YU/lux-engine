#pragma once
// Explicit, bounded test input tracing, compiled only into the diagnostic candidate.
#include <cstdio>
#include <cstdlib>
#include <string_view>
namespace lux::diagnostics
{
    inline bool selectedTextTraceEnabled() noexcept
    {
        const auto *value = std::getenv("LUX_ER1_SELECTED_TEXT_TRACE");
        return value && std::string_view{value} == "1";
    }
    inline void traceCodepoint(const char *stage, unsigned codepoint, bool glyph = false) noexcept
    {
        static unsigned count{};
        const bool selected = codepoint == 0x4F60 || codepoint == 0x597D || codepoint == 0x3002 ||
                              codepoint == 0xFF0C || codepoint == 0xFFFD || codepoint == '?' ||
                              codepoint == 'A' || codepoint == 'n' || codepoint == 'i';
        if (selected && count < 64 && selectedTextTraceEnabled())
        {
            ++count;
            std::fprintf(stderr, "ER1 selected_text stage=%s codepoint=U+%04X glyph_present=%d\n",
                         stage, codepoint, glyph);
        }
    }
    inline void traceFilter(std::string_view value) noexcept
    {
        static unsigned count{};
        const bool selected = value.empty() || value == "\xE4\xBD\xA0" || value == "\xE5\xA5\xBD" ||
                              value == "\xE3\x80\x82" || value == "\xEF\xBC\x8C" || value == "A" ||
                              value == "?" || value == "\xEF\xBF\xBD";
        if (selected && count < 64 && selectedTextTraceEnabled())
        {
            ++count;
            std::fprintf(stderr, "ER1 selected_text stage=Filter bytes=%zu utf8=", value.size());
            for (unsigned char byte : value)
                std::fprintf(stderr, "%02X", unsigned(byte));
            std::fputc('\n', stderr);
        }
    }
}
