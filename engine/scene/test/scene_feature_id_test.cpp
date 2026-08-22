#include <lux/engine/scene/SceneFeatureId.hpp>

#include <cassert>
#include <type_traits>

namespace
{
    struct OtherIdTag final {};
    using OtherId = lux::cxx::StableNameId<OtherIdTag>;
}

int main()
{
    using namespace lux::scene;

    static_assert(!std::is_same_v<SceneFeatureId, OtherId>);
    static_assert(sceneFeatureId("org.lux.scene.physics3d").isValid());

    assert(isValidSceneFeatureIdName("org.lux.scene.physics3d"));
    assert(!isValidSceneFeatureIdName("Org.lux.scene.physics3d"));
    assert(!isValidSceneFeatureIdName("org..lux"));
    assert(!isValidSceneFeatureIdName("scene"));

    const SceneFeatureId owned{"org.lux.scene.physics3d"};
    assert(sameSceneFeatureId(
        owned.view(),
        sceneFeatureId("org.lux.scene.physics3d")));
    return 0;
}
