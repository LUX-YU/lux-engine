#include <lux/engine/resource/asset/codecs/AssetCodecCatalog.hpp>
#include <lux/engine/scene/SceneAsset.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>

int main()
{
    const auto catalog = lux::scene::makeSceneAssetCodecCatalog(
        *lux::asset::runtimeAssetCodecCatalog());
    if (!catalog ||
        (*catalog)->findByMagic(lux::scene::kSceneAssetMagic) == nullptr)
    {
        return 1;
    }

    lux::scene::SceneDescription description;
    description.id = uuids::uuid::from_string(
        "11111111-2222-4333-8444-555555555555").value();
    const auto encoded = lux::scene::SceneAssetSerDeser::encodeData(
        description.id,
        description);
    if (!encoded)
        return 2;
    const auto decoded = lux::scene::SceneAssetSerDeser::decodeData(*encoded);
    return decoded && (*decoded)->id == description.id ? 0 : 3;
}
