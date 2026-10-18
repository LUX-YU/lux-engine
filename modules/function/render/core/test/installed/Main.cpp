#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/CppType.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>

int main()
{
    using namespace lux::render;
    static_assert(cppType<int>().size == sizeof(int));
    static_assert(cppType<int>() != cppType<unsigned>());
    lux::cxx::SlotMap<int, SceneTag> scenes;
    const SceneId old = scenes.insert(42);
    if (!scenes.erase(old)) return 1;
    const SceneId replacement = scenes.insert(43);
    if (scenes.find(old) || old == replacement) return 2;
    TargetDesc target{800, 600, lux::rdesc::ETextureFormat::RGBA8_UNORM};
    if (!target.validate()) return 3;
    target.width = 0;
    const auto invalid = target.validate();
    if (invalid || invalid.error().type != kInvalidTargetExtent) return 4;
    if (renderCoreErrorDescriptors().size() != 5) return 5;
    RasterViewport viewport;
    if (!viewport.validate()) return 6;
    viewport.width = 0;
    const auto invalid_viewport = viewport.validate();
    if (invalid_viewport || invalid_viewport.error().type != kInvalidViewport) return 7;
    return 0;
}
