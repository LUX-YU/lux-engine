#pragma once

#include "Asset.hpp"
#include "AssetSerDeser.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <lux/engine/resource/visibility.h>

namespace lux::asset
{
    /**
     * @brief Forward declaration of the AssetManagerImpl class.
     */
    class AssetManagerImpl;

    class AssetVfs;

    /// Opaque token returned by onLoaded / onWillUnload. Use with unsubscribe()
    /// to cancel a subscription before it fires. Tokens are unique per
    /// AssetManager instance; zero is never a valid live token.
    using SubscriptionToken = std::uint64_t;
    inline constexpr SubscriptionToken kInvalidSubscriptionToken = 0;

    /// Fired when an asset transitions from absent to present in the manager
    /// — i.e. when registerAsset() succeeds for the matching id. If the asset
    /// is already present when onLoaded() is called, the callback fires
    /// synchronously before onLoaded() returns. The callback is single-shot:
    /// it auto-unsubscribes after firing.
    ///
    /// Receives the asset id and a non-null pointer to the now-present asset.
    /// The pointer is borrowed — do NOT keep it past the callback; use an
    /// AssetHandle<T> instead to track lifecycle automatically.
    using AssetLoadedCallback = std::function<void(asset_id_t, LuxAsset*)>;

    /// Fired immediately before removeAsset() actually evicts the asset's
    /// data. All subscribers run synchronously; after they return the asset
    /// pointer becomes invalid. Subscribers MUST release any cached pointer /
    /// GPU handle / index they hold for this asset here. Single-shot: auto-
    /// unsubscribes after firing.
    using AssetUnloadCallback = std::function<void(asset_id_t)>;

    /**
     * @brief Manages assets and resources throughout the application lifecycle.
     *
     * The AssetManager provides a centralized interface for loading, storing, and retrieving
     * assets. It handles asset lifetime management, caching, and provides type-safe access
     * to different asset types such as textures, materials, meshes, and models.
     */
    class LUX_RESOURCE_PUBLIC AssetManager
    {
        // Grant access to AssetSerDeser for serialization/deserialization.
        friend class AssetSerDeser;

    public:
        /**
         * @brief Constructs a new AssetManager instance with default settings.
         */
        AssetManager();

        /**
         * @brief Destructs the AssetManager instance and releases all managed resources.
         */
        ~AssetManager();

        // Copy operations are deleted to enforce unique asset management.
        AssetManager(const AssetManager&) = delete;
        AssetManager& operator=(const AssetManager&) = delete;

        /**
         * @brief Template function to query asset data by asset ID.
         *
         * This is a convenience wrapper that calls the non-template queryData internally.
         *
         * @tparam T The expected type of asset data.
         * @param id The unique asset identifier.
         * @return Pointer to the asset data of type T.
         */
        template<typename T>
        T* queryData(const asset_id_t& id)
        {
            return queryData(id);
        }

        /**
         * @brief Checks if asset data exists for the given asset ID.
         *
         * @param id The unique asset identifier.
         * @return true if the asset data exists, false otherwise.
         */
        [[nodiscard]] bool hasData(const asset_id_t& id) const;

        /**
         * @brief Checks if an asset with the given asset ID exists.
         *
         * @param id The unique asset identifier.
         * @return true if the asset exists, false otherwise.
         */
        [[nodiscard]] bool hasAsset(const asset_id_t& id) const;

        /**
         * @brief Retrieves constant asset information for a given asset ID.
         *
         * @param id The unique asset identifier.
         * @return Pointer to the constant AssetInfo.
         */
        [[nodiscard]] const AssetInfo* queryInfo(const asset_id_t& id) const;

        /**
         * @brief Fetches a mutable asset using its asset ID.
         *
         * @param id The unique asset identifier.
         * @return Pointer to the asset.
         */
        [[nodiscard]] LuxAsset* fetchAsset(const asset_id_t& id);
        /**
         * @brief Fetches a constant asset using its asset ID.
         *
         * @param id The unique asset identifier.
         * @return Pointer to the constant asset.
         */
        [[nodiscard]] const LuxAsset* fetchAsset(const asset_id_t& id) const;

        /**
         * @brief Fetches an asset and casts it to the specified type.
         *
         * Returns nullptr if the asset does not exist or the type does not match.
         *
         * @tparam T The type derived from LuxAsset.
         * @param id The unique asset identifier.
         * @return Pointer to the asset of type T, or nullptr on failure.
         */
        template<typename T>
        T* fetchAssetAs(const asset_id_t& id)
            requires std::is_base_of_v<LuxAsset, T>
        {
            LuxAsset* asset = fetchAsset(id);
            if (asset == nullptr)
                return nullptr;

            if (asset->type() != T::asset_type)
                return nullptr;

            return static_cast<T*>(asset);
        }

        /**
         * @brief Fetches a constant asset and casts it to the specified type.
         *
         * Returns nullptr if the asset does not exist or the type does not match.
         *
         * @tparam T The type derived from LuxAsset.
         * @param id The unique asset identifier.
         * @return Pointer to the constant asset of type T, or nullptr on failure.
         */
        template<typename T>
        const T* fetchAssetAs(const asset_id_t& id) const
            requires std::is_base_of_v<LuxAsset, T>
        {
            const LuxAsset* asset = fetchAsset(id);
            if (asset == nullptr)
                return nullptr;

            if (asset->type() != T::asset_type)
                return nullptr;

            return static_cast<const T*>(asset);
        }

        /**
         * @brief Queries asset information and casts it to the specified type.
         *
         * Returns nullptr if asset information is not found.
         *
         * @tparam T The type derived from AssetInfo.
         * @param id The unique asset identifier.
         * @return Pointer to asset information of type T, or nullptr on failure.
         */
        template<typename T>
        T* queryInfoAs(const asset_id_t& id) const
            requires std::is_base_of_v<AssetInfo, T>
        {
            return static_cast<T*>(queryInfo(id));
        }

        /**
         * @brief Generates a unique asset identifier (UUID).
         *
         * @return The generated asset_id_t.
         */
        [[nodiscard]] asset_id_t generateUUID() const;

        /**
         * @brief Generates a deterministic (name-based) UUID from @p seed.
         *
         * The same seed always maps to the same UUID under a fixed engine
         * namespace. The import pipeline feeds a content-relative identity
         * (e.g. "Models/CesiumMan|mesh|0") so re-importing a source reproduces
         * identical ids and the on-disk reference graph survives. Runtime asset
         * creation should keep using the random generateUUID() overload.
         *
         * @param seed Stable identity string for the asset.
         * @return The deterministic asset_id_t.
         */
        [[nodiscard]] asset_id_t generateUUID(std::string_view seed) const;

        /**
         * @brief Creates a new AssetInfo instance for the given asset type.
         *
         * The function automatically sets the current asset version, generates a new UUID, assigns
         * the asset type, and records the current timestamp.
         *
         * @param type The asset type.
         * @return A unique_ptr to the newly created AssetInfo.
         */
        std::unique_ptr<AssetInfo> createAssetInfo(EAssetType type) const
        {
            return createAssetInfo(type, std::string_view{});
        }

        /**
         * @brief Creates a new AssetInfo, optionally with a deterministic id.
         *
         * When @p seed is non-empty the id is derived deterministically from it
         * (see generateUUID(std::string_view)); when empty, a random id is used
         * — so existing callers keep their behavior unchanged.
         *
         * @param type The asset type.
         * @param seed Stable identity string, or empty for a random id.
         * @return A unique_ptr to the newly created AssetInfo.
         */
        std::unique_ptr<AssetInfo> createAssetInfo(EAssetType type, std::string_view seed) const
        {
            auto current_time = std::chrono::system_clock::now().time_since_epoch();
            // Convert the current time to nanoseconds.
            auto date = std::chrono::duration_cast<std::chrono::nanoseconds>(current_time).count();

            auto info = std::make_unique<AssetInfo>();

            info->id = seed.empty() ? generateUUID() : generateUUID(seed);
            info->type = type;
            info->date = date;

			return info;
        }

        template<typename T, typename... Args>
        std::unique_ptr<LuxAsset> createAsset(std::unique_ptr<typename T::asset_data_t> data, Args&&... args)
        {
			auto info  = createAssetInfo(T::asset_type);
			auto asset = std::make_unique<T>(
                std::move(info),
                std::forward<Args>(args)...
            );
			asset->setData(std::move(data));
			return asset;
        }

        /**
         * @brief Like createAsset, but derives a deterministic id from @p seed.
         *
         * An empty seed falls back to a random id (identical to createAsset), so
         * this is a safe drop-in for import paths that may or may not request
         * determinism.
         */
        template<typename T, typename... Args>
        std::unique_ptr<LuxAsset> createAssetSeeded(std::string_view seed,
            std::unique_ptr<typename T::asset_data_t> data, Args&&... args)
        {
			auto info  = createAssetInfo(T::asset_type, seed);
			auto asset = std::make_unique<T>(
                std::move(info),
                std::forward<Args>(args)...
            );
			asset->setData(std::move(data));
			return asset;
        }

        /**
         * @brief Registers a new asset with the AssetManager.
         *
         * Transfers ownership of the asset via a std::unique_ptr.
         *
         * @param asset The asset to register.
         * @return true if the asset was successfully registered, false otherwise.
         */
        bool registerAsset(std::unique_ptr<LuxAsset> asset);

        /**
         * @brief Remove (evict) an asset from the manager, freeing its CPU memory.
         *
         * Fires every onWillUnload subscriber for @p id synchronously BEFORE
         * the asset is destroyed, so subscribers can release the cached
         * pointer they hold (the AssetHandle<T> RAII wrapper does this
         * automatically). After this call returns the asset's CPU data is
         * gone and any raw pointer obtained via fetchAsset / queryData is
         * invalid. If no asset exists for the id, this is a no-op (no
         * callbacks fire).
         *
         * @param id The asset to remove.
         */
        void removeAsset(const asset_id_t& id);

        /**
         * @brief Drop an asset's CPU data back to a SHELL (keep it registered).
         *
         * 大世界 W2c eviction: unlike removeAsset (which deletes the asset and
         * dangles its UUID), this keeps the info-only shell registered and only
         * frees the decoded CPU data — so the next requestLoad(id) re-decodes it.
         * No callback fires (the shell stays present; consumers re-query by id and
         * tolerate a null data() — the renderable bridge / anim resolver already
         * do). Guarded inside the asset: a no-op (returns false) if the asset is
         * absent, already data-less, or FROZEN_FOR_UPLOAD (a GPU upload is
         * borrowing the bytes). Main-thread only (same contract as registerAsset).
         *
         * @return true if data was dropped; false if no-op / unsafe.
         */
        [[nodiscard]] bool unloadData(const asset_id_t& id);

        // ─── Lifecycle subscription ────────────────────────────────────────
        //
        // The two callbacks below let consumers react to asset arrival /
        // departure without polling the manager every frame. The intended
        // entry point for application code is AssetHandle<T> (see
        // AssetHandle.hpp) — a move-only RAII wrapper that combines both
        // subscriptions into a single typed accessor.

        /**
         * @brief Subscribe to "asset became present" for @p id.
         *
         * If the asset is already in the manager at call time, @p cb fires
         * synchronously before returning AND the subscription is consumed —
         * the returned token still survives for unsubscribe() but no further
         * fire happens. Otherwise the callback is queued until a matching
         * registerAsset() lands, at which point it fires once then auto-
         * unsubscribes.
         *
         * @return Token usable with unsubscribe(); never zero unless OOM.
         */
        SubscriptionToken onLoaded(const asset_id_t& id, AssetLoadedCallback cb);

        /**
         * @brief Subscribe to "asset about to be removed" for @p id.
         *
         * Fires once during removeAsset(@p id) — synchronously, before the
         * asset's data is freed — then auto-unsubscribes. If the asset is
         * never removed during the subscription's lifetime, the callback
         * simply never fires (call unsubscribe() to cancel proactively).
         *
         * @return Token usable with unsubscribe(); never zero unless OOM.
         */
        SubscriptionToken onWillUnload(const asset_id_t& id, AssetUnloadCallback cb);

        /**
         * @brief Cancel a pending subscription. No-op if @p token has already
         *        fired or was unsubscribed before.
         */
        void unsubscribe(SubscriptionToken token);

        // ─── Virtual filesystem (id -> locator, replacing id -> file) ──────
        //
        // The manager stays a pure in-memory id->asset map; the VFS is the
        // pluggable answer to "where do absent assets come from". The editor
        // mounts LooseDirProvider over the content folder; shipped builds
        // mount PakAssetProvider — same manager code either way.

        /**
         * @brief Attach (or detach with nullptr) the asset VFS used by
         *        findAssetByPath / ensureAsset.
         */
        void setVfs(std::shared_ptr<const AssetVfs> vfs);

        /**
         * @brief The attached VFS, or nullptr.
         */
        [[nodiscard]] std::shared_ptr<const AssetVfs> vfs() const;

        /**
         * @brief Resolve an absolute virtual path ("/Game/Materials/M_Box")
         *        to an asset id. PURE resolution — never loads, never touches
         *        disk beyond the provider's in-memory tables. Nil id when no
         *        VFS is attached, the path is illegal, or nothing matches.
         */
        [[nodiscard]] asset_id_t findAssetByPath(std::string_view vpath) const;

        /**
         * @brief Get-or-load: returns the present asset, or loads it through
         *        the VFS (open -> magic dispatch -> SerDeser -> registerAsset,
         *        firing onLoaded subscribers) and returns it.
         *
         * Main-thread contract (same as registerAsset). Errors:
         * ASSET_NOT_EXIST when absent and no VFS / no provider has the id;
         * otherwise the provider's open error or the SerDeser's parse error.
         */
        [[nodiscard]] lux::cxx::expected<LuxAsset*, EAssetError>
        ensureAsset(const asset_id_t& id);

    private:
        /**
         * @brief Internal function to query asset data.
         *
         * Returns a void pointer to the asset data associated with the given asset ID.
         *
         * @param id The unique asset identifier.
         * @return Void pointer to the asset data.
         */
        void* queryData(const asset_id_t& id);

        /**
         * @brief Internal function to query an asset by its identifier.
         *
         * @param id The unique asset identifier.
         * @return Pointer to the asset.
         */
        LuxAsset* queryAsset(const asset_id_t& id);
        /**
         * @brief Const version of queryAsset.
         *
         * @param id The unique asset identifier.
         * @return Pointer to the constant asset.
         */
        const LuxAsset* queryAsset(const asset_id_t& id) const;

        /**
         * @brief Pointer to the internal implementation of AssetManager.
         *
         * Uses the Pimpl idiom to hide implementation details.
         */
        std::unique_ptr<AssetManagerImpl> impl_;
    };
}
