// ============================================================================
//  spatial_cull_grid_test.cpp — 工作流① 增量2 W1 cell 数学自检 (CPU-only)
//
//  Pins SpatialCullGrid's pure cell math without a Vulkan device:
//    1. cellCoord  — world XY → integer cell coord (floor, incl. negatives).
//    2. cellInRange — a cell is active iff SOME camera is within
//       (cull_distance + one-cell slack), measured in 2D XY (cells span full Z;
//       empty cameras → active so the partition never over-culls).
//
//  The GPU side (a dormant-cell instance vanishing from the cull dispatch, so the
//  active-instance count drops as the camera pulls away) is validated in-editor.
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/render/scene/SpatialCullGrid.hpp>

#include <array>
#include <cstdint>
#include <cstdio>

using namespace lux::render;

namespace
{
    int g_fails = 0;
    void check(bool ok, const char* what)
    {
        if (!ok) { std::printf("  FAIL: %s\n", what); ++g_fails; }
    }

    bool cellIs(float x, float y, float cs, int32_t ex, int32_t ey)
    {
        int32_t cx = 0, cy = 0;
        SpatialCullGrid::cellCoord(x, y, cs, cx, cy);
        return cx == ex && cy == ey;
    }
}

int main()
{
    // ---- 1. cellCoord: world XY → integer cell (floor, incl. negatives) ----
    {
        check(cellIs(   0.f,    0.f, 100.f,  0,  0), "origin -> (0,0)");
        check(cellIs(  99.f,   99.f, 100.f,  0,  0), "(99,99) -> (0,0)");
        check(cellIs( 100.f,  100.f, 100.f,  1,  1), "(100,100) -> (1,1)");
        check(cellIs( 250.f,   50.f, 100.f,  2,  0), "(250,50) -> (2,0)");
        check(cellIs(  -1.f,   -1.f, 100.f, -1, -1), "(-1,-1) -> (-1,-1) [floor negatives]");
        check(cellIs(-100.f, -100.f, 100.f, -1, -1), "(-100,-100) -> (-1,-1)");
        check(cellIs(-101.f, -101.f, 100.f, -2, -2), "(-101,-101) -> (-2,-2)");
    }

    // ---- 2. cellInRange: ground-plane X-Z distance vs (cull_distance + one cell) ----
    // cell_size=100, cull_distance=200 → eff = 300. cell (0,0) centre = (X50,Z50).
    // Cameras are {x,y,z}: the planar coords are x + z; Y/height is spanned.
    {
        const float cs = 100.f, lr = 200.f;

        // empty cameras → keep active (never over-cull).
        check(SpatialCullGrid::cellInRange(0, 0, cs, lr, {}) == true,
              "no camera -> active");

        const std::array<std::array<float, 3>, 1> on_cell{{ {{50.f, 0.f, 50.f}} }};
        check(SpatialCullGrid::cellInRange(0, 0, cs, lr, on_cell) == true,
              "camera on cell -> active");

        // dist to (X50,Z50): camera (x50,z400) -> 350 > 300 -> dormant.
        const std::array<std::array<float, 3>, 1> far{{ {{50.f, 0.f, 400.f}} }};
        check(SpatialCullGrid::cellInRange(0, 0, cs, lr, far) == false,
              "camera 350 away -> dormant");

        // just inside the slack: (x50,z340) -> 290 < 300.
        const std::array<std::array<float, 3>, 1> inside{{ {{50.f, 0.f, 340.f}} }};
        check(SpatialCullGrid::cellInRange(0, 0, cs, lr, inside) == true,
              "camera 290 away -> active (within slack)");

        // just outside: (x50,z360) -> 310 > 300.
        const std::array<std::array<float, 3>, 1> outside{{ {{50.f, 0.f, 360.f}} }};
        check(SpatialCullGrid::cellInRange(0, 0, cs, lr, outside) == false,
              "camera 310 away -> dormant");

        // multi-camera: ANY one in range -> active (the union, editor multi-viewport).
        const std::array<std::array<float, 3>, 2> multi{{ {{50.f, 0.f, 400.f}}, {{50.f, 0.f, 50.f}} }};
        check(SpatialCullGrid::cellInRange(0, 0, cs, lr, multi) == true,
              "one of two cameras in range -> active");

        // Y/height is ignored (cells span Y): a camera high above but on X-Z stays active.
        const std::array<std::array<float, 3>, 1> high_y{{ {{50.f, 99999.f, 50.f}} }};
        check(SpatialCullGrid::cellInRange(0, 0, cs, lr, high_y) == true,
              "camera high in Y only -> active (X-Z plane)");

        // a distant cell with the same near-XY camera: cell (10,0) centre=(1050,50),
        // dist to (50,50) = 1000 >> 300 -> dormant (the coarse-cull payoff).
        check(SpatialCullGrid::cellInRange(10, 0, cs, lr, on_cell) == false,
              "far cell, near camera -> dormant");
    }

    if (g_fails == 0)
        std::printf("spatial_cull_grid_test: PASS\n");
    else
        std::printf("spatial_cull_grid_test: FAIL (%d)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
