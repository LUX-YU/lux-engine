#include <lux/engine/scene/ScenePackageCodec.hpp>

int main()
{
    // An empty package is intentionally invalid. The purpose of this probe is
    // to force one call through the public shared-library boundary while
    // linking only scene_package and its declared transitive dependencies.
    lux::scene::ScenePackage package;
    const auto validated = lux::scene::validateScenePackage(package);
    return validated ? 1 : 0;
}
