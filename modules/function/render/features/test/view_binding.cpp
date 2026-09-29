#include <lux/engine/render/renderer/features/highlight/HighlightFeature.hpp>
#include <lux/engine/render/renderer/features/grid/Grid3DPassFeature.hpp>
#include <cassert>
#include <iostream>
using namespace lux::render;
int main()
{
    // These are the actual backend Feature classes used by command handlers and recording kernels.
    HighlightFeature backend{HighlightFeature::Config{}};
    Grid3DPassFeature grid{Grid3DPassFeature::Config{}};
    const ViewHandle first{0, 1}, second{1, 1}, recycled{0, 2};
    const auto a = static_cast<ERenderEntityId>(31), b = static_cast<ERenderEntityId>(47);
    backend.replaceTargets(first, {a, a});
    backend.replaceTargets(second, {b});
    assert(backend.targets(first).size() == 1 && backend.targets(first)[0] == a);
    assert(backend.targets(second).size() == 1 && backend.targets(second)[0] == b);
    assert(backend.targets(recycled).empty());
    Grid3DParams x{}, y{};
    x.planeY = 10;
    y.planeY = 20;
    grid.setGrid3DParams(first, x);
    grid.setGrid3DParams(second, y);
    assert(grid.params(first).planeY == 10 && grid.params(second).planeY == 20);
    backend.deallocateViewState(first.index);
    grid.deallocateViewState(first.index);
    assert(backend.targets(first).empty() && backend.targets(second)[0] == b);
    assert(grid.params(first).planeY == 0 && grid.params(second).planeY == 20);
    backend.replaceTargets(recycled, {a});
    assert(backend.targets(first).empty());
    std::cout << "X07-03 actual backend HighlightFeature and Grid3DPassFeature: full view identity, independent "
                 "selection/plane, retirement and slot generation. GPU pixels remain P10/P13 qualification.\n";
}
