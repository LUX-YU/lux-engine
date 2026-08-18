#include <lux/engine/function/render/RenderEffectId.hpp>

#include <cassert>
#include <type_traits>

int main()
{
    using namespace lux::render;

    static_assert(!std::is_same_v<RenderEffectId, RenderEffectIdView>);
    constexpr auto grid = renderEffectId("org.lux.render.grid3d.effect");
    static_assert(grid.isValid());

    assert(isValidRenderEffectIdName(grid.name()));
    assert(!isValidRenderEffectIdName("Org.lux.render.invalid"));
    assert(!isValidRenderEffectIdName("org..lux.render.invalid"));

    const RenderEffectId owned{grid.name()};
    assert(owned.view() == grid);
    return 0;
}
