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
        std::uint32_t magic,
        std::uint32_t legacy_magic = 0u,
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
            magic,
            legacy_magic,
            nullptr,
            std::move(lifetime)};
    }
}

int main()
{
    using namespace lux::asset;

    auto duplicate_type = AssetCodecCatalog::build({
        descriptor(EAssetType::MESH, 1u, "MeshA", 101u),
        descriptor(EAssetType::MESH, 2u, "MeshB", 102u)});
    if (duplicate_type || duplicate_type.error() !=
            EAssetCodecCatalogError::DUPLICATE_ASSET_TYPE)
    {
        return 1;
    }

    auto collision = AssetCodecCatalog::build({
        descriptor(EAssetType::MESH, 7u, "MeshA", 101u),
        descriptor(EAssetType::TEXTURE, 7u, "TextureB", 102u)});
    if (collision || collision.error() !=
            EAssetCodecCatalogError::TYPE_HASH_COLLISION)
    {
        return 2;
    }

    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<const void> observed = lifetime;
    {
        auto built = AssetCodecCatalog::build({
            descriptor(EAssetType::MESH, 1u, "Mesh", 101u, 0u, lifetime),
            descriptor(EAssetType::TEXTURE, 2u, "Texture", 102u)});
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
        runtime->findByMagic(
            asset_magic_number_of<EAssetType::TEXTURE>::value) == nullptr)
    {
        return 5;
    }

    AssetManager manager{runtime};
    if (manager.codecCatalogOwner() != runtime)
        return 6;

    auto duplicate_magic = AssetCodecCatalog::build({
        descriptor(EAssetType::MESH, 1u, "Mesh", 101u),
        descriptor(EAssetType::TEXTURE, 2u, "Texture", 101u)});
    if (duplicate_magic || duplicate_magic.error() !=
            EAssetCodecCatalogError::DUPLICATE_MAGIC)
    {
        return 7;
    }

    auto duplicate_name = AssetCodecCatalog::build({
        descriptor(EAssetType::MESH, 1u, "SharedName", 101u),
        descriptor(EAssetType::TEXTURE, 2u, "SharedName", 102u)});
    if (duplicate_name || duplicate_name.error() !=
            EAssetCodecCatalogError::DUPLICATE_CPP_TYPE)
    {
        return 8;
    }

    auto legacy_collision = AssetCodecCatalog::build({
        descriptor(EAssetType::MESH, 1u, "Mesh", 101u, 201u),
        descriptor(EAssetType::TEXTURE, 2u, "Texture", 201u)});
    if (legacy_collision || legacy_collision.error() !=
            EAssetCodecCatalogError::DUPLICATE_MAGIC)
    {
        return 9;
    }

    auto legacy_catalog = AssetCodecCatalog::build({
        descriptor(EAssetType::MESH, 1u, "Mesh", 101u, 201u),
        descriptor(EAssetType::TEXTURE, 2u, "Texture", 102u)});
    if (!legacy_catalog ||
        legacy_catalog->findByMagic(101u) == nullptr ||
        legacy_catalog->findByMagic(201u) == nullptr ||
        legacy_catalog->findByMagic(201u)->type != EAssetType::MESH ||
        legacy_catalog->findByMagic(999u) != nullptr)
    {
        return 10;
    }
    return 0;
}
