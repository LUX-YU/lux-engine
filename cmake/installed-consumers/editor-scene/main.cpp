#include <lux/engine/editor/scene/SceneCamera.hpp>
#include <lux/engine/editor/scene/SceneWorkbench.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <cassert>

int main()
{
    lux::meta::ReflectionRegistry::initRegistry();
    {
        auto meta = lux::editor::workbench::buildDevelopmentSceneMeta();
        assert(meta);
        lux::editor::workbench::SceneCamera camera;
        const auto before = camera.position();
        camera.focus({0, 0, 0}, 2);
        assert((camera.position() - before).norm() > 0);
        assert(camera.projection(1.5).allFinite());
        assert(camera.view(Eigen::Vector3d::Zero()).allFinite());
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
