#include <AssetManagerImpl.hpp>
#include <lux/engine/asset/AssetVfs.hpp>
#include <lux/engine/asset/AssetHeaderProbe.hpp>

#include <cstring>

namespace lux::asset
{
    /**
     * @brief Constructs an AssetManager with default implementation.
     */
    AssetManager::AssetManager() {
        impl_ = std::make_unique<AssetManagerImpl>();
    }

    /**
     * @brief Destructor for AssetManager.
     */
    AssetManager::~AssetManager() = default;

    /**
     * @brief Checks if asset data exists for the given ID.
     * @param id The unique identifier of the asset to check
     * @return true if asset data exists, false otherwise
     */
    bool AssetManager::hasData(const uuids::uuid& id) const
    {
        return impl_->hasData(id);
    }

    /**
     * @brief Checks if an asset exists for the given ID.
     * @param id The unique identifier of the asset to check
     * @return true if the asset exists, false otherwise
     */
	bool AssetManager::hasAsset(const uuids::uuid& id) const
    {
        return impl_->hasAsset(id);
    }

    /**
     * @brief Queries asset information for the given ID.
     * @param id The unique identifier of the asset
     * @return Pointer to AssetInfo if found, nullptr otherwise
     */
	const AssetInfo* AssetManager::queryInfo(const uuids::uuid& id) const
    {
        return impl_->queryInfo(id);
    }

    /**
     * @brief Generates a new unique UUID for asset identification.
     * @return A newly generated UUID
     */
    uuids::uuid AssetManager::generateUUID() const
    {
        return impl_->generateAssetId();
    }

    /**
     * @brief Generates a deterministic name-based UUID from a seed string.
     * @param seed Stable identity string for the asset.
     * @return A UUID derived deterministically from the seed.
     */
    uuids::uuid AssetManager::generateUUID(std::string_view seed) const
    {
        return impl_->generateAssetId(seed);
    }

    /**
     * @brief Registers a new asset with the manager.
     * @param asset Unique pointer to the asset to register
     * @return true if registration was successful, false otherwise
     */
    bool AssetManager::registerAsset(std::unique_ptr<LuxAsset> asset)
    {
        return impl_->registerAsset(std::move(asset));
    }

    /**
     * @brief Remove (evict) an asset from the manager, freeing its CPU memory.
     * @param id The unique identifier of the asset to remove
     */
    void AssetManager::removeAsset(const asset_id_t& id)
    {
        impl_->removeAsset(id);
    }

    bool AssetManager::unloadData(const asset_id_t& id)
    {
        return impl_->unloadData(id);
    }

    /**
     * @brief Queries an asset by its unique identifier.
     * @param id The unique identifier of the asset
     * @return Pointer to the asset if found, nullptr otherwise
     */
    LuxAsset* AssetManager::queryAsset(const uuids::uuid& id)
    {
        return impl_->queryAsset(id);
    }

    const LuxAsset* AssetManager::queryAsset(const uuids::uuid& id) const
    {
        return impl_->queryAsset(id);
    }

    LuxAsset* AssetManager::fetchAsset(const asset_id_t& id)
    {
        return impl_->fetchAsset(id);
    }

    const LuxAsset* AssetManager::fetchAsset(const asset_id_t& id) const
    {
		return impl_->fetchAsset(id);
    }

    SubscriptionToken AssetManager::onLoaded(const asset_id_t& id, AssetLoadedCallback cb)
    {
        return impl_->onLoaded(id, std::move(cb));
    }

    SubscriptionToken AssetManager::onWillUnload(const asset_id_t& id, AssetUnloadCallback cb)
    {
        return impl_->onWillUnload(id, std::move(cb));
    }

    void AssetManager::unsubscribe(SubscriptionToken token)
    {
        impl_->unsubscribe(token);
    }

    void AssetManager::setVfs(std::shared_ptr<const AssetVfs> vfs)
    {
        impl_->setVfs(std::move(vfs));
    }

    std::shared_ptr<const AssetVfs> AssetManager::vfs() const
    {
        return impl_->vfs();
    }

    asset_id_t AssetManager::findAssetByPath(std::string_view vpath) const
    {
        const auto& vfs = impl_->vfs();
        return vfs ? vfs->resolve(vpath) : asset_id_t{};
    }

    lux::cxx::expected<LuxAsset*, EAssetError>
    AssetManager::ensureAsset(const asset_id_t& id)
    {
        if (id.is_nil())
            return lux::cxx::unexpected(EAssetError::ASSET_NOT_EXIST);

        if (auto* present = queryAsset(id))
            return present;

        const auto& vfs = impl_->vfs();
        if (!vfs)
            return lux::cxx::unexpected(EAssetError::ASSET_NOT_EXIST);

        auto blob = vfs->open(id);
        if (!blob.has_value())
            return lux::cxx::unexpected(blob.error());
        if (blob.value().size < sizeof(std::uint32_t))
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        std::uint32_t magic = 0;
        std::memcpy(&magic, blob.value().data.get(), sizeof(magic));
        auto serdeser = createSerDeserFor(
            assetTypeOfMagic(magic),
            // Non-owning aliasing shared_ptr: the SerDeser lives only for
            // this call and self-registers into *this. AssetManager is also
            // constructed on the stack (tests), so enable_shared_from_this
            // is not an option here.
            std::shared_ptr<AssetManager>(std::shared_ptr<AssetManager>{}, this)
        );
        if (!serdeser)
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED);

        auto loaded =
            serdeser->fromLuxAssetMemory(blob.value().data.get(), blob.value().size);
        if (!loaded.has_value())
        {
            // Lost a race with a same-frame load — the asset is there now.
            if (loaded.error() == EAssetError::ASSET_ALREADY_EXIST)
            {
                if (auto* present = queryAsset(id))
                    return present;
            }
            return lux::cxx::unexpected(loaded.error());
        }

        if (loaded.value().second != id)
        {
            // The image carries a different identity than the locator that
            // produced it — a corrupt index or a moved file. Don't keep the
            // impostor registered under its own id.
            removeAsset(loaded.value().second);
            return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);
        }
        return loaded.value().first;
    }
}
