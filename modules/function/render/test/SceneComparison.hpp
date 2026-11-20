#pragma once

#include "SceneCapture.hpp"

static void verifyEqualPixels(const CapturedPixels& pixels, bool expect_lit = false)
{
    unsigned observed = 0;
    for (const auto& frame : pixels.frames)
    {
        std::uint32_t marker = 0;
        std::memcpy(&marker, frame.data(), 4);
        if (!marker)
        {
            continue;
        }
        bool saw_lit_pixel = false;
        for (std::size_t i = 0; i < 64 * 64; ++i)
        {
            std::memcpy(&marker, frame.data() + i * 4, 4);
            const bool matched = marker == 1 || marker == 3;
            if (!matched)
            {
                std::fprintf(stderr, "HDR mismatch pixel=%zu marker=%u\n", i, marker);
            }
            CHECK(matched);
            saw_lit_pixel |= marker == 3;
        }
        CHECK(!expect_lit || saw_lit_pixel);
        ++observed;
    }
    CHECK(observed);
}
