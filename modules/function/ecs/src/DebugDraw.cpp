#include <lux/engine/ecs/DebugDraw.hpp>

#include <vector>

namespace
{
    // Half the LineList feature's default vertex capacity (200'000 verts /
    // 2 verts per line) — draws beyond the cap are dropped, never resized
    // into an upload the feature would truncate anyway.
    constexpr std::size_t kMaxLines = 100'000;

    // Retained store. MAIN THREAD ONLY (see header contract) — no locking.
    std::vector<lux::ecs::debugdraw::DebugLine> g_lines;
} // namespace

namespace lux::ecs::debugdraw
{
    std::span<const DebugLine> lines() noexcept
    {
        return g_lines;
    }

    void clear() noexcept
    {
        g_lines.clear();
    }

} // namespace lux::ecs::debugdraw

void lux_debug_draw_line(float x0, float y0, float z0,
                         float x1, float y1, float z1,
                         float r, float g, float b)
{
    if (g_lines.size() >= kMaxLines)
    {
        return;
    }
    g_lines.push_back(lux::ecs::debugdraw::DebugLine{
        { x0, y0, z0 }, { x1, y1, z1 }, { r, g, b } });
}

void lux_debug_draw_clear()
{
    lux::ecs::debugdraw::clear();
}
