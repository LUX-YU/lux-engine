#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/CppType.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/core/Camera.hpp>

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
    CameraState camera;
    if (!camera.validate() || cameraParameters(camera).size() != 128) return 6;
    camera.viewport.width = 0;
    const auto bad_camera = camera.validate();
    if (bad_camera || bad_camera.error().type != kInvalidCamera) return 7;
    return 0;
}
