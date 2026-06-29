//=============================================================================
// asset_lifecycle_test.cpp
// -----------------------------------------------------------------------------
// Coverage for AssetManager::onLoaded / onWillUnload / unsubscribe — the
// lifecycle subscription API.
//
// (The AssetHandle<T> RAII wrapper that USED to build on these callbacks was
// retired in the async-asset redesign: consumers now re-query the manager via
// fetchAssetAs<T>(id) each frame rather than holding a lifecycle-subscribed
// pointer, so this test no longer exercises a handle — just the raw callbacks
// the manager still exposes.)
//
// Always-on (no external assets needed):
//   * onLoaded fires when registerAsset lands later, single-shot.
//   * onLoaded fires synchronously when the asset is already present.
//   * onWillUnload fires inside removeAsset, before the pointer dies.
//   * unsubscribe before fire silences the callback.
//=============================================================================

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/SkeletonAsset.hpp>
#include <lux/engine/description/Skeleton.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <utility>

using namespace lux::asset;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& desc)
{
    if (cond) { std::cout << "  PASS  " << desc << "\n"; ++g_pass; }
    else      { std::cout << "  FAIL  " << desc << "\n"; ++g_fail; }
}

static void banner(const char* title)
{
    std::cout << "\n" << std::string(60, '=') << "\n"
              << "  " << title << "\n"
              << std::string(60, '=') << "\n";
}

// Build a SkeletonAsset whose AssetInfo carries the provided id. We bypass
// AssetManager::createAsset (which auto-generates an id) because the tests
// want deterministic ids to subscribe against before registration.
static std::unique_ptr<SkeletonAsset>
makeSkeletonAssetWithId(asset_id_t id)
{
    auto info  = std::make_unique<AssetInfo>();
    info->id   = id;
    info->type = SkeletonAsset::asset_type;
    info->date = 0;

    auto data  = std::make_unique<lux::rdesc::Skeleton>();
    // One dummy bone so hasData() returns true and the skeleton looks
    // reasonable to consumers; the lifecycle tests below don't care about
    // bone contents.
    lux::rdesc::Bone_t bone;
    bone.name = "root";
    bone.parent_index = -1;
    bone.bind_local = Eigen::Affine3f::Identity();
    bone.inv_bind_world = Eigen::Affine3f::Identity();
    data->bones.push_back(std::move(bone));

    auto asset = std::make_unique<SkeletonAsset>(std::move(info));
    asset->setData(std::move(data));
    return asset;
}

//-----------------------------------------------------------------------------
// Bare callbacks (no AssetHandle yet)
//-----------------------------------------------------------------------------
static void test_subscribe_then_register()
{
    banner("Test 1: onLoaded fires when registerAsset arrives later");

    AssetManager mgr;
    const asset_id_t id = mgr.generateUUID();

    int fire_count = 0;
    LuxAsset* observed = nullptr;
    auto token = mgr.onLoaded(id,
        [&](asset_id_t /*ev*/, LuxAsset* asset) {
            ++fire_count;
            observed = asset;
        });
    check(token != kInvalidSubscriptionToken, "onLoaded returns live token");
    check(fire_count == 0,                    "callback NOT fired before register");

    mgr.registerAsset(makeSkeletonAssetWithId(id));
    check(fire_count == 1,                    "callback fired exactly once on register");
    check(observed != nullptr,                "callback received non-null asset pointer");
    check(observed != nullptr && observed->type() == EAssetType::SKELETON,
                                              "observed asset has correct type");

    // Single-shot: a second register (after remove) should not refire the
    // already-consumed subscription.
    mgr.removeAsset(id);
    mgr.registerAsset(makeSkeletonAssetWithId(id));
    check(fire_count == 1, "auto-unsubscribed after single fire (no refire on re-register)");
}

static void test_register_then_subscribe()
{
    banner("Test 2: onLoaded fires synchronously when asset already present");

    AssetManager mgr;
    const asset_id_t id = mgr.generateUUID();
    mgr.registerAsset(makeSkeletonAssetWithId(id));

    int fire_count = 0;
    auto token = mgr.onLoaded(id, [&](asset_id_t, LuxAsset*) { ++fire_count; });
    check(token != kInvalidSubscriptionToken,
          "onLoaded returns live token even for already-present asset");
    check(fire_count == 1, "callback fired synchronously inside onLoaded()");
}

static void test_will_unload_fires_in_remove()
{
    banner("Test 3: onWillUnload fires once, before the asset goes away");

    AssetManager mgr;
    const asset_id_t id = mgr.generateUUID();
    mgr.registerAsset(makeSkeletonAssetWithId(id));

    int fire_count = 0;
    bool asset_still_alive_at_callback = false;
    mgr.onWillUnload(id, [&](asset_id_t cb_id) {
        ++fire_count;
        // The asset MUST still be reachable inside the callback; that's the
        // whole point — subscribers get one last chance to read it before
        // the manager drops it.
        asset_still_alive_at_callback = mgr.fetchAsset(cb_id) != nullptr;
    });
    check(fire_count == 0, "callback NOT fired before removeAsset");

    mgr.removeAsset(id);
    check(fire_count == 1,                   "callback fired exactly once");
    check(asset_still_alive_at_callback,     "asset still reachable inside callback");
    check(mgr.fetchAsset(id) == nullptr,     "asset gone after removeAsset returns");

    // Removing a non-existent id is a no-op (no callback, no crash).
    fire_count = 0;
    mgr.removeAsset(id);
    check(fire_count == 0, "removing absent asset is a no-op (no double-fire)");
}

static void test_unsubscribe_before_fire()
{
    banner("Test 4: unsubscribe() silences pending callbacks");

    AssetManager mgr;
    const asset_id_t id = mgr.generateUUID();

    int loaded_fires = 0;
    int unload_fires = 0;
    auto loaded_tok = mgr.onLoaded(id,    [&](asset_id_t, LuxAsset*) { ++loaded_fires; });
    auto unload_tok = mgr.onWillUnload(id, [&](asset_id_t)            { ++unload_fires; });

    mgr.unsubscribe(loaded_tok);
    mgr.unsubscribe(unload_tok);

    mgr.registerAsset(makeSkeletonAssetWithId(id));
    mgr.removeAsset(id);

    check(loaded_fires == 0, "onLoaded callback silenced");
    check(unload_fires == 0, "onWillUnload callback silenced");

    // Unsubscribing an unknown / already-fired token is a no-op.
    mgr.unsubscribe(loaded_tok);
    mgr.unsubscribe(SubscriptionToken{99999999u});
    mgr.unsubscribe(kInvalidSubscriptionToken);
    check(true, "double / stale unsubscribe is harmless");
}

//-----------------------------------------------------------------------------
int main()
{
    std::cout << "Asset lifecycle subscription acceptance test\n"
              << "============================================\n";

    test_subscribe_then_register();
    test_register_then_subscribe();
    test_will_unload_fires_in_remove();
    test_unsubscribe_before_fire();

    std::cout << "\n" << std::string(60, '=') << "\n"
              << "  Summary: " << g_pass << " passed, "
                                << g_fail << " failed\n"
              << std::string(60, '=') << "\n";
    return g_fail == 0 ? 0 : 1;
}
