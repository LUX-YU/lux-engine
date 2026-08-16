#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>

#include <memory>
#include <string>
#include <vector>

namespace
{
    std::unique_ptr<lux::asset::AssetSerDeser> dummyFactory(
        lux::asset::EAssetType,
        std::shared_ptr<lux::asset::AssetManager>)
    {
        return nullptr;
    }

    lux::asset::AssetCodecDescriptor descriptor(
        lux::asset::EAssetType type,
        std::uint64_t hash,
        std::string name,
        std::shared_ptr<const void> lifetime = {})
    {
        return lux::asset::AssetCodecDescriptor{
            type,
            hash,
            std::move(name),
            lux::asset::EAssetShippingClass::RUNTIME,
            &dummyFactory,
            nullptr,
            nullptr,
            std::move(lifetime)};
    }
}

int main()
{
    using namespace lux::asset;

    auto duplicate_type = AssetCodecCatalog::build({
        descriptor(EAssetType::MESH, 1u, "MeshA"),
        descriptor(EAssetType::MESH, 2u, "MeshB")});
    if (duplicate_type || duplicate_type.error() !=
            EAssetCodecCatalogError::DUPLICATE_ASSET_TYPE)
    {
        return 1;
    }

    auto collision = AssetCodecCatalog::build({
        descriptor(EAssetType::MESH, 7u, "MeshA"),
        descriptor(EAssetType::TEXTURE, 7u, "TextureB")});
    if (collision || collision.error() !=
            EAssetCodecCatalogError::TYPE_HASH_COLLISION)
    {
        return 2;
    }

    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<const void> observed = lifetime;
    {
        auto built = AssetCodecCatalog::build({
            descriptor(EAssetType::MESH, 1u, "Mesh", lifetime),
            descriptor(EAssetType::TEXTURE, 2u, "Texture")});
        lifetime.reset();
        if (!built || observed.expired() ||
            built->find(EAssetType::MESH) == nullptr ||
            built->find(EAssetType::SHADER) != nullptr)
        {
            return 3;
        }
    }
    if (!observed.expired())
        return 4;

    const auto runtime = runtimeAssetCodecCatalog();
    if (!runtime || runtime->find(EAssetType::TEXTURE) == nullptr ||
        runtime->find(EAssetType::FLOW_GRAPH) != nullptr ||
        runtime->find(EAssetType::ENTITY_SCENE) != nullptr ||
        runtime->find(EAssetType::ENTITY_SECTION) != nullptr)
    {
        return 5;
    }

    AssetManager manager{runtime};
    if (manager.codecCatalogOwner() != runtime)
        return 6;
    return 0;
}
