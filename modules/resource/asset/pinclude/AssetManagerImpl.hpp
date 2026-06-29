#pragma once
#include <lux/engine/asset/AssetManager.hpp>
#include <unordered_map>
#include <utility>
#include <vector>
#include <lux/engine/asset/TextureSerDeser.hpp>
#include <lux/engine/asset/MeshSerDeser.hpp>
#include <lux/engine/asset/ShaderSerDeser.hpp>
#include <lux/engine/asset/ModelSerDeser.hpp>

#include <random>
#include <string_view>

namespace lux::asset
{
    // Fixed namespace uuid for all deterministic (name-based, RFC-4122 v5)
    // asset ids. This value is part of the on-disk id derivation and MUST NOT
    // change once assets ship — altering it re-rolls every imported uuid. (The
    // bytes were generated once at random; this is not any well-known uuid.)
    inline constexpr uuids::uuid kEngineAssetNamespace{
        std::array<std::uint8_t, 16>{
            0x9a, 0x1d, 0x4c, 0x7e, 0x2b, 0x86, 0x53, 0xf1,
            0xa0, 0x4e, 0x6d, 0x3c, 0x12, 0x7f, 0x88, 0x55 } };

    class AssetManagerImpl
    {
    public:
        AssetManagerImpl()
        {
            auto seed_data = std::array<int, std::mt19937::state_size> {};
            std::ranges::generate(seed_data, std::ref(rd_));
            std::seed_seq seq(std::begin(seed_data), std::end(seed_data));
            generator_ = std::mt19937(seq);
            id_generator_ = new uuids::uuid_random_generator(generator_);
        }

        ~AssetManagerImpl()
        {
            delete id_generator_;
        }

        asset_id_t generateAssetId() const
        {
            return id_generator_->operator()();
        }

        // Deterministic, name-based (RFC-4122 v5 / SHA-1) id under the fixed
        // engine namespace: the same @p seed always yields the same uuid. The
        // import pipeline uses this so re-importing a source reproduces the
        // exact same ids and the on-disk reference graph survives — random ids
        // would orphan it on every re-import. A fresh generator per call keeps
        // this method free of mutable state (uuid_name_generator::operator()
        // is non-const).
        asset_id_t generateAssetId(std::string_view seed) const
        {
            uuids::uuid_name_generator gen{ kEngineAssetNamespace };
            return gen(seed);
        }

        void removeAsset(const asset_id_t& id)
        {
            // Asset must exist for a callback fire to make sense; if it
            // doesn't, we still drop any orphan onWillUnload subscriptions
            // (no-op for typical paths since handles unsubscribe in their
            // dtor before the asset is removed).
            auto asset_it = assets_.find(id);
            if (asset_it == assets_.end())
            {
                // Drop any orphan unload subscriptions silently.
                eraseSubscriptions(unload_subs_, id);
                return;
            }

            // Fire onWillUnload subscribers synchronously before the asset's
            // data goes away. Move the bucket out first so a callback that
            // touches AssetManager (e.g. unsubscribes a sibling) cannot
            // invalidate the iterator we are walking.
            fireAndDrain(unload_subs_, id, [&](const UnloadSub& sub) {
                if (sub.cb) sub.cb(id);
            });

            // After all callbacks ran the asset pointer is still live; now
            // free it.
            assets_.erase(asset_it);

            // onLoaded subscriptions queued against this id but never fired
            // would otherwise dangle. Drop them — re-register will create
            // fresh ones if the asset reappears later.
            eraseSubscriptions(load_subs_, id);
        }

        [[nodiscard]] bool hasData(const asset_id_t& id) const
        {
            if (!hasAsset(id))
                return false;

            return assets_.at(id)->hasData();
        }

        // W2c: drop the asset's CPU data back to a shell (keep it registered).
        // The FROZEN_FOR_UPLOAD / already-empty guard lives in LuxAsset::unloadData.
        bool unloadData(const asset_id_t& id)
        {
            auto it = assets_.find(id);
            if (it == assets_.end())
                return false;
            return it->second->unloadData();
        }

		[[nodiscard]] bool hasAsset(const asset_id_t& id) const
        {
            return assets_.contains(id);
        }

		[[nodiscard]] const AssetInfo* queryInfo(const asset_id_t& id) const
        {
            if (!hasAsset(id))
                return nullptr;

            return assets_.at(id)->info();
        }

        bool registerAsset(std::unique_ptr<LuxAsset> asset)
        {
            if (asset->info() == nullptr)
                return false;

            const asset_id_t id = asset->info()->id;
            if (hasAsset(id))
                return false;

            auto [it, ok] = assets_.emplace(id, std::move(asset));
            if (!ok) return false;

            // Fire any onLoaded subscribers that were queued waiting for this
            // id. Drains the bucket — single-shot semantics. fireAndDrain
            // takes the bucket out before invoking callbacks, so a callback
            // that re-subscribes against the same id (creating a new bucket)
            // doesn't accidentally fire twice for this same arrival.
            LuxAsset* asset_ptr = it->second.get();
            fireAndDrain(load_subs_, id, [&](const LoadSub& sub) {
                if (sub.cb) sub.cb(id, asset_ptr);
            });
            return true;
        }

        // -------- Lifecycle subscriptions (A1 of architecture cleanup) --------

        SubscriptionToken onLoaded(const asset_id_t& id, AssetLoadedCallback cb)
        {
            // Asset already present → fire synchronously and skip queueing.
            // The returned token is still allocated so callers (esp.
            // AssetHandle's ctor path) can unconditionally unsubscribe later;
            // unsubscribe of an unknown token is a no-op.
            if (auto it = assets_.find(id); it != assets_.end())
            {
                const SubscriptionToken token = ++next_token_;
                if (cb) cb(id, it->second.get());
                return token;
            }
            const SubscriptionToken token = ++next_token_;
            load_subs_.push_back(LoadSub{token, id, std::move(cb)});
            return token;
        }

        SubscriptionToken onWillUnload(const asset_id_t& id, AssetUnloadCallback cb)
        {
            const SubscriptionToken token = ++next_token_;
            unload_subs_.push_back(UnloadSub{token, id, std::move(cb)});
            return token;
        }

        void unsubscribe(SubscriptionToken token)
        {
            if (token == kInvalidSubscriptionToken) return;
            // O(N) scan across both lists — acceptable: subscription counts
            // scale with live AssetHandle count, which is bounded by live
            // ECS entities, not by per-frame churn. If this ever shows up
            // in a profile, swap in a token → (list, index) lookup table.
            for (auto it = load_subs_.begin(); it != load_subs_.end(); ++it)
                if (it->token == token) { load_subs_.erase(it); return; }
            for (auto it = unload_subs_.begin(); it != unload_subs_.end(); ++it)
                if (it->token == token) { unload_subs_.erase(it); return; }
        }

        LuxAsset* queryAsset(const asset_id_t& id)
        {
            if (assets_.contains(id) == false)
                return nullptr;

            return assets_.at(id).get();
        }

        const LuxAsset* queryAsset(const asset_id_t& id) const
        {
            if (assets_.contains(id) == false)
                return nullptr;

            return assets_.at(id).get();
        }

        LuxAsset* fetchAsset(const asset_id_t& id)
        {
			if (assets_.contains(id) == false)
				return nullptr;
			return assets_.at(id).get();
        }

        const LuxAsset* fetchAsset(const asset_id_t& id) const
        {
            if (assets_.contains(id) == false)
                return nullptr;
            return assets_.at(id).get();
        }

    private:
        struct LoadSub   { SubscriptionToken token; asset_id_t id; AssetLoadedCallback cb; };
        struct UnloadSub { SubscriptionToken token; asset_id_t id; AssetUnloadCallback cb; };

        // Take every subscription matching @p id out of @p subs, then invoke
        // @p fire on each. Extracting first means a callback that touches the
        // same list (re-subscribe, unsubscribe sibling) cannot disturb the
        // iteration; it also implements the single-shot "auto unsubscribe
        // after firing" contract.
        template<typename SubList, typename Fn>
        static void fireAndDrain(SubList& subs, const asset_id_t& id, Fn&& fire)
        {
            SubList taken;
            taken.reserve(4);
            for (auto it = subs.begin(); it != subs.end();)
            {
                if (it->id == id)
                {
                    taken.push_back(std::move(*it));
                    it = subs.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            for (auto& sub : taken)
                fire(sub);
        }

        template<typename SubList>
        static void eraseSubscriptions(SubList& subs, const asset_id_t& id)
        {
            std::erase_if(subs, [&](const auto& sub) { return sub.id == id; });
        }

    public:
        void setVfs(std::shared_ptr<const AssetVfs> vfs) { vfs_ = std::move(vfs); }
        [[nodiscard]] const std::shared_ptr<const AssetVfs>& vfs() const { return vfs_; }

    private:
        std::shared_ptr<const AssetVfs>                            vfs_;
        std::random_device                                         rd_;
        std::mt19937                                               generator_;
        uuids::uuid_random_generator*                              id_generator_;
        std::unordered_map<asset_id_t, std::unique_ptr<LuxAsset>>  assets_;

        // Lifecycle subscriptions. std::vector instead of multimap because
        // the typical live count is small (one per consumer per asset, not
        // per frame), and linear scans dominate over hash overhead at that
        // scale. Tokens are monotonically allocated and never reused — so a
        // stale unsubscribe is harmless.
        std::vector<LoadSub>     load_subs_;
        std::vector<UnloadSub>   unload_subs_;
        SubscriptionToken        next_token_{0};
    };
}