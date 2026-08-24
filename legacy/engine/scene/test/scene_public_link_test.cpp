#include <lux/engine/scene/SceneAssetSerDeser.hpp>

int main()
{
    // An empty package is intentionally invalid. The purpose of this probe is
    // to force one call through the public shared-library boundary while
    // linking only scene and its declared transitive dependencies.
    lux::scene::SceneDescription package;
    const auto validated = lux::scene::validateSceneDescription(package);
    return validated ? 1 : 0;
}
