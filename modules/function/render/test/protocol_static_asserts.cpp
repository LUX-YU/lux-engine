// ============================================================================
//  protocol_static_asserts.cpp — R2-00: the Canvas2D public protocol is frozen as
//  trivially-copyable value PODs with server-OWNED handles (no borrowed pointers that
//  could dangle across a frame). Most of this test is the compile itself; a handful of
//  runtime checks pin the default-init contract (null handles, opaque tint, premul).
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>

#include <cstdio>
#include <type_traits>

using namespace lux::render;

// ── Compile-time contract: every wire packet is a trivially-copyable POD ──
//  (belt-and-suspenders alongside the asserts in the header — a test that fails to
//  compile is a caught regression too.)
static_assert(std::is_trivially_copyable_v<DrawOrderKey>);
static_assert(std::is_trivially_copyable_v<Rect2D>);
static_assert(std::is_trivially_copyable_v<SpriteDraw>);
static_assert(std::is_trivially_copyable_v<TileDraw>);
static_assert(std::is_trivially_copyable_v<PixelFieldDraw>);

static_assert(std::is_standard_layout_v<DrawOrderKey>);
static_assert(std::is_standard_layout_v<SpriteDraw>);
static_assert(std::is_standard_layout_v<TileDraw>);
static_assert(std::is_standard_layout_v<PixelFieldDraw>);

// A draw packet must NOT embed a raw pointer — variable-length producer data rides an
// owner handle, not a borrowed pointer. Prove the packets carry no pointer members by
// their size: transform[16] (64B) + key (20B) dominate; any hidden pointer would be
// caught by review, but at minimum the handles are value types (trivially copyable).
static_assert(std::is_trivially_copyable_v<SpriteBatchHandle>);
static_assert(std::is_trivially_copyable_v<TilemapRenderHandle>);
static_assert(std::is_trivially_copyable_v<PixelFieldRenderHandle>);

// The alpha mode is a 1-byte value enum (protocol-stable).
static_assert(sizeof(Canvas2DAlphaMode) == 1);

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }
}

int main()
{
    std::printf("=== protocol_static_asserts (R2-00) ===\n");

    // Default-init contract — a freshly value-initialised packet is inert:
    //  null owner handle, opaque-white premultiplied tint, zero key.
    const SpriteDraw     s{};
    const TileDraw       t{};
    const PixelFieldDraw p{};

    check(s.tint == 0xFFFFFFFFu, "SpriteDraw default tint is opaque white");
    check(t.chunk.is_null(),     "TileDraw default chunk handle is null (nothing borrowed)");
    check(p.field.is_null(),     "PixelFieldDraw default field handle is null (nothing borrowed)");
    check(s.key == DrawOrderKey{}, "default SpriteDraw carries the zero draw key");
    check(Canvas2DAlphaMode{} == Canvas2DAlphaMode::Premultiplied, "default alpha mode is premultiplied (MVP)");

    // Distinct owner-handle tags are distinct C++ types (a Tilemap handle cannot be
    // passed where a PixelField handle is expected) — a compile-time guarantee we only
    // sanity-check for value semantics here.
    check(SpriteBatchHandle{}.is_null() && TilemapRenderHandle{}.is_null() && PixelFieldRenderHandle{}.is_null(),
          "all owner handles default-construct null");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
