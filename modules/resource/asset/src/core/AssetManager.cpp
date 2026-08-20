#include <lux/engine/resource/asset/detail/AssetManagerImpl.hpp>
#include <lux/engine/resource/asset/AssetVfs.hpp>

#include <cstring>
#include <cstdlib>

namespace lux::asset
{
    /**
     * @brief Constructs an AssetManager with default implementation.
     */
    AssetManager::AssetManager(std::shared_ptr<const AssetCodecCatalog> codecs)
    {
        if (!codecs)
            std::abort();
        impl_ = std::make_unique<AssetManagerImpl>(std::move(codecs));
    }

    /**
     * @brief Destructor for AssetManager.
     */
    AssetManager::~AssetManager() = default;

    std::unique_ptr<AssetSerDeser> AssetManager::createSerDeser(
        EAssetType type,
        std::shared_ptr<AssetManager> owner) const
    {
        const auto& codecs = impl_->codecCatalogOwner();
        return codecs ? codecs->create(type, std::move(owner)) : nullptr;
    }

    const AssetCodecCatalog& AssetManager::codecCatalog() const noexcept
    {
        return *impl_->codecCatalogOwner();
    }

    std::shared_ptr<const AssetCodecCatalog>
    AssetManager::codecCatalogOwner() const noexcept
    {
        return impl_->codecCatalogOwner();
    }

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

    void AssetManager::retain(const asset_id_t& id) { impl_->retain(id); }

    std::uint32_t AssetManager::release(const asset_id_t& id) { return impl_->release(id); }

    void AssetManager::setBroadcast(BroadcastCallbacks callbacks)
    {
        impl_->setBroadcast(std::move(callbacks));
    }

    std::uint32_t AssetManager::contentRevision(const asset_id_t& id) const
    {
        return impl_->contentRevision(id);
    }

    void AssetManager::notifyContentChanged(const asset_id_t& id)
    {
        impl_->bumpContentRevision(id);
    }

    bool AssetManager::replaceAsset(std::unique_ptr<LuxAsset> asset)
    {
        return impl_->replaceAsset(std::move(asset));
    }

    AssetRef AssetManager::acquire(const asset_id_t& id)
    {
        if (id.is_nil()) return {};
        impl_->retain(id);
        return AssetRef(this, id);
    }

    // ── AssetRef 的票据生命周期(声明见 AssetRef.hpp)───────────────────────
    //
    // 出线定义是刻意的:AssetRef.hpp 只前置声明 AssetManager,组件头包含它时
    // 不背上资产序列化的重量;计数操作发生在组件换装/离场的频率上,非热路径。

    AssetRef::AssetRef(const AssetRef& other)
        : mgr_(other.mgr_), id_(other.id_)
    {
        if (mgr_ && !id_.is_nil()) mgr_->retain(id_);
    }

    AssetRef& AssetRef::operator=(const AssetRef& other)
    {
        if (this != &other)
        {
            // 「先钉新再放旧」:自我同 id 赋值时不得先归零 —— 那会假广播一次
            // 零边沿,派生缓存据此销毁一个马上又要用的 GPU 副本。
            AssetManager* old_mgr = mgr_;
            const asset_id_t old_id = id_;
            mgr_ = other.mgr_;
            id_  = other.id_;
            if (mgr_ && !id_.is_nil()) mgr_->retain(id_);
            if (old_mgr && !old_id.is_nil()) (void)old_mgr->release(old_id);
        }
        return *this;
    }

    AssetRef& AssetRef::operator=(AssetRef&& other) noexcept
    {
        if (this != &other)
        {
            // 接管 other 的那一票(计数不动),再还自己原来的 —— 同上,先新后旧。
            AssetManager* old_mgr = mgr_;
            const asset_id_t old_id = id_;
            mgr_ = other.mgr_;
            id_  = other.id_;
            other.mgr_ = nullptr;
            other.id_  = {};
            if (old_mgr && !old_id.is_nil()) (void)old_mgr->release(old_id);
        }
        return *this;
    }

    AssetRef::~AssetRef() { reset(); }

    void AssetRef::reset() noexcept
    {
        if (mgr_ && !id_.is_nil()) (void)mgr_->release(id_);
        mgr_ = nullptr;
        id_  = {};
    }

    bool AssetManager::isReferenced(const asset_id_t& id) const noexcept
    {
        return impl_->isReferenced(id);
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
        {
            if (present->hasData())
                return present;

            // Shell 无 data(unloadData 驱逐后)。此前这里直接把空壳交回去 ——
            // 对所有同步兜底路径(Android 的 request_load 就是本方法、动画/
            // 缓存的无 executor 兜底)驱逐是**单向**的:数据永远不会回来。
            // 照 AssetLoadService 的安装形状同步重解码装回壳(纯解码,不再注册 ——
            // 走 fromLuxAssetMemory 会撞 ALREADY_EXIST)。失败时交回壳照旧
            // (消费者本来就按 hasData 判,容忍空壳是既有契约)。
            const auto& shell_vfs = impl_->vfs();
            if (shell_vfs)
            {
                if (auto blob = shell_vfs->open(id);
                    blob.has_value() &&
                        blob->bytes.size() >= sizeof(std::uint32_t))
                {
                    if (auto inj = codecCatalog().decode(blob->bytes);
                        inj.has_value())
                        inj.value()(*present);
                }
            }
            return present;
        }

        const auto& vfs = impl_->vfs();
        if (!vfs)
            return lux::cxx::unexpected(EAssetError::ASSET_NOT_EXIST);

        auto blob = vfs->open(id);
        if (!blob.has_value())
            return lux::cxx::unexpected(blob.error());
        if (blob->bytes.size() < sizeof(std::uint32_t))
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        std::uint32_t magic = 0;
        std::memcpy(&magic, blob->bytes.data(), sizeof(magic));
        const auto* descriptor = codecCatalog().findByMagic(magic);
        if (descriptor == nullptr)
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED);
        auto serdeser = createSerDeser(
            descriptor->type,
            // Non-owning aliasing shared_ptr: the SerDeser lives only for
            // this call and self-registers into *this. AssetManager is also
            // constructed on the stack (tests), so enable_shared_from_this
            // is not an option here.
            std::shared_ptr<AssetManager>(std::shared_ptr<AssetManager>{}, this)
        );
        if (!serdeser)
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED);

        auto loaded =
            serdeser->fromLuxAssetMemory(
                blob->bytes.data(), blob->bytes.size());
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
