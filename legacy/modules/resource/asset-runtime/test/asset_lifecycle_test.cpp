//=============================================================================
// asset_lifecycle_test.cpp
// -----------------------------------------------------------------------------
// Coverage for AssetManager::setBroadcast — the ledger's broadcast callback
// seam (统一事件系统批E:三队列退役成回调缝;批E2 连 onLoaded/onWillUnload
// 订阅面一起退役 —— AssetHandle 时代遗物,生产使用面归零)。
//
// Always-on (no external assets needed):
//   * removeAsset 不清账(裁决七);归零回调恰在最后一票析构时发一次。
//   * removeAsset 发 invalidated,不伪造归零;缺席 id 是 no-op。
//   * replaceAsset/notifyContentChanged 发 content_changed(带 bump 后的
//     revision),不动计数、不发失效;removeAsset 反之(语义互不混用)。
//=============================================================================

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/description/Skeleton.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <cstdint>

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
makeSkeletonAssetWithId(
    asset_id_t id,
    EAssetType type = SkeletonAsset::asset_type
)
{
    auto info  = std::make_unique<AssetInfo>();
    info->id   = id;
    info->type = type;
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
static void test_registered_broadcast()
{
    banner("Test 8: on_registered broadcast (驻留 T2 —— 解封的推式信号)");

    // 「删了又回来」的失效封印解除,此前靠消费端每帧 queryInfo 轮询;
    // on_registered 给它一个推式边沿。契约:注册成功恰好一次;重复 id
    // 拒收不触发;replaceAsset 不触发(对象在场,那是 content_changed)。
    AssetManager mgr{runtimeAssetCodecCatalog()};
    const asset_id_t id = mgr.generateUUID();

    std::vector<asset_id_t> registered;
    std::vector<asset_id_t> content_changed;
    mgr.setBroadcast({.on_content_changed =
                          [&](const asset_id_t& c, std::uint32_t) { content_changed.push_back(c); },
                      .on_registered =
                          [&](const asset_id_t& r) { registered.push_back(r); }});

    check(mgr.registerAsset(makeSkeletonAssetWithId(id)), "register succeeds");
    check(registered.size() == 1 && registered[0] == id,
          "on_registered fires exactly once on success");

    check(!mgr.registerAsset(makeSkeletonAssetWithId(id)),
          "duplicate id refused");
    check(registered.size() == 1, "refused registration does NOT broadcast");

    check(mgr.replaceAsset(makeSkeletonAssetWithId(id)), "replace succeeds");
    check(registered.size() == 1,
          "replaceAsset does NOT fire on_registered (object was present)");
    check(content_changed.size() == 1 && content_changed[0] == id,
          "replaceAsset routes to content_changed as before");

    // 删了又回来:invalidated 语义之后,重注册再响一次 —— 这就是解封边沿。
    mgr.removeAsset(id);
    check(mgr.registerAsset(makeSkeletonAssetWithId(id)), "re-register after remove");
    check(registered.size() == 2 && registered[1] == id,
          "re-registration fires again — the unseal edge");

    mgr.setBroadcast({});
}

//-----------------------------------------------------------------------------
static void test_remove_asset_keeps_ledger()
{
    banner("Test 5: removeAsset does NOT clear the residency ledger (决七前提)");

    // 兴趣比对象活得久:资产对象死后,持票者随后松手时计数照常流干、归零广播
    // 照常触发 —— 派生 GPU 副本靠那个边沿回收。removeAsset 若清账,还在持有的
    // AssetRef 释放时就等不到归零边沿,GPU 副本必漏。本测试钉住这条契约。
    AssetManager mgr{runtimeAssetCodecCatalog()};
    const asset_id_t id = mgr.generateUUID();
    mgr.registerAsset(makeSkeletonAssetWithId(id));

    // 批E:三队列退役 —— 广播是回调缝(账本线程同步调),模块单测直接接
    // 回调,零总线依赖(总线接线是宿主组装层的事)。
    std::vector<asset_id_t> zeroed;
    mgr.setBroadcast({.on_unreferenced =
                          [&](const asset_id_t& z) { zeroed.push_back(z); }});
    {
        AssetRef ticket = mgr.acquire(id);
        check(mgr.isReferenced(id), "acquire pins the id");

        mgr.removeAsset(id);
        check(mgr.fetchAsset(id) == nullptr, "asset object gone");
        check(mgr.isReferenced(id),
              "ledger entry SURVIVES removeAsset (interest outlives the object)");
        check(zeroed.empty(),
              "no zero-edge broadcast while the ticket is still held");
    }   // ticket 析构:1→0
    check(!mgr.isReferenced(id), "count drains when the holder lets go");
    check(zeroed.size() == 1 && zeroed[0] == id,
          "zero-edge broadcast fires exactly once, AFTER removal");
    mgr.setBroadcast({});
}

static void test_loaded_asset_installation()
{
    banner("Test 9: decoded assets install without hot-reload side effects");

    AssetManager mgr{runtimeAssetCodecCatalog()};
    const asset_id_t id = mgr.generateUUID();
    std::vector<asset_id_t> registered;
    std::vector<asset_id_t> changed;
    mgr.setBroadcast({
        .on_content_changed =
            [&](const asset_id_t& value, std::uint32_t)
            { changed.push_back(value); },
        .on_registered =
            [&](const asset_id_t& value) { registered.push_back(value); },
    });

    auto initial = mgr.installLoadedAsset(
        id,
        makeSkeletonAssetWithId(id)
    );
    check(initial && *initial == mgr.fetchAsset(id),
          "first decoded object is registered");
    check(registered == std::vector{id},
          "first decoded object emits the ordinary registration edge");
    check(mgr.contentRevision(id) == 0u && changed.empty(),
          "initial installation is not reported as a content edit");

    AssetRef ticket = mgr.acquire(id);
    check(mgr.unloadData(id) && !mgr.hasData(id),
          "resident object becomes a data-less shell");
    auto reloaded = mgr.installLoadedAsset(
        id,
        makeSkeletonAssetWithId(id)
    );
    check(reloaded && mgr.hasData(id),
          "complete decode replaces a data-less shell");
    check(mgr.isReferenced(id) && mgr.contentRevision(id) == 0u &&
              registered.size() == 1u && changed.empty(),
          "shell replacement preserves tickets, revision, and broadcasts");

    LuxAsset* installed = mgr.fetchAsset(id);
    auto duplicate = mgr.installLoadedAsset(
        id,
        makeSkeletonAssetWithId(id)
    );
    check(duplicate && *duplicate == installed && mgr.fetchAsset(id) == installed,
          "duplicate completion keeps the already-ready object");

    const asset_id_t other = mgr.generateUUID();
    auto wrong_id = mgr.installLoadedAsset(
        other,
        makeSkeletonAssetWithId(id)
    );
    check(!wrong_id && wrong_id.error() == EAssetError::WRONG_FILE_HEADER,
          "expected and decoded IDs must match");

    const asset_id_t unknown_type_id = mgr.generateUUID();
    auto unknown_type = mgr.installLoadedAsset(
        unknown_type_id,
        makeSkeletonAssetWithId(unknown_type_id, EAssetType::UNKNOWN)
    );
    check(!unknown_type && unknown_type.error() == EAssetError::FILE_TYPE_ERROR,
          "decoded type must be registered in the manager catalog");

    ticket.reset();
    mgr.setBroadcast({});
}

static void test_asset_ref_value_semantics()
{
    banner("Test 5b: AssetRef copy/move/reset keeps the single ledger balanced");

    AssetManager mgr{runtimeAssetCodecCatalog()};
    const asset_id_t id = mgr.generateUUID();
    mgr.registerAsset(makeSkeletonAssetWithId(id));
    std::vector<asset_id_t> zeroed;
    mgr.setBroadcast({.on_unreferenced =
        [&](const asset_id_t& value) { zeroed.push_back(value); }});

    AssetRef first = mgr.acquire(id);
    AssetRef copy = first;
    check(first && copy && mgr.isReferenced(id),
          "copy acquires a second ledger ticket");

    first.reset();
    check(first.empty() && copy && mgr.isReferenced(id) && zeroed.empty(),
          "reset releases only its own ticket");

    AssetRef moved = std::move(copy);
    check(copy.empty() && moved && mgr.isReferenced(id),
          "move transfers without changing the ledger");

    AssetRef assigned;
    assigned = moved;
    moved.reset();
    check(assigned && mgr.isReferenced(id) && zeroed.empty(),
          "copy assignment retains before the source releases");

    assigned = std::move(assigned);
    check(assigned && mgr.isReferenced(id),
          "self move assignment preserves the ticket");
    assigned.reset();
    assigned.reset();
    check(!mgr.isReferenced(id) && zeroed.size() == 1u && zeroed[0] == id,
          "last reset emits one 1-to-0 edge and repeated reset is idempotent");
    mgr.setBroadcast({});
}

static void test_invalidation_broadcast()
{
    banner("Test 6: removeAsset broadcasts INVALIDATION (决七的第二事件)");

    AssetManager mgr{runtimeAssetCodecCatalog()};
    const asset_id_t id = mgr.generateUUID();
    mgr.registerAsset(makeSkeletonAssetWithId(id));

    std::vector<asset_id_t> invalidated, zeroed;
    mgr.setBroadcast({
        .on_unreferenced = [&](const asset_id_t& z) { zeroed.push_back(z); },
        .on_invalidated  = [&](const asset_id_t& v) { invalidated.push_back(v); },
    });
    check(invalidated.empty(), "no invalidation before removal");

    mgr.removeAsset(id);
    check(invalidated.size() == 1 && invalidated[0] == id,
          "removeAsset fires the invalidation callback");
    check(zeroed.empty(),
          "invalidation does NOT forge a zero-edge (两条语义分离)");

    // Removing an absent id is a no-op — no ghost invalidation.
    mgr.removeAsset(id);
    check(invalidated.size() == 1, "absent removeAsset broadcasts nothing");
    mgr.setBroadcast({});
}

static void test_content_changed_broadcast()
{
    banner("Test 7: content-changed 第三队列(热更新批4)—— 与前两条语义分离");

    AssetManager mgr{runtimeAssetCodecCatalog()};
    const asset_id_t id = mgr.generateUUID();
    mgr.registerAsset(makeSkeletonAssetWithId(id));

    std::vector<std::pair<asset_id_t, std::uint32_t>> changed;
    std::vector<asset_id_t> zeroed, invalidated;
    mgr.setBroadcast({
        .on_unreferenced = [&](const asset_id_t& z) { zeroed.push_back(z); },
        .on_invalidated  = [&](const asset_id_t& v) { invalidated.push_back(v); },
        .on_content_changed =
            [&](const asset_id_t& c, std::uint32_t rev)
            { changed.emplace_back(c, rev); },
    });
    check(mgr.contentRevision(id) == 0, "revision starts at 0");
    check(changed.empty(), "no change before anything happened");

    // ── replaceAsset:同 id 换对象 → bump + 广播;不动计数、不推失效。──
    AssetRef ticket = mgr.acquire(id);
    check(mgr.replaceAsset(makeSkeletonAssetWithId(id)), "replace succeeds for a registered id");
    check(mgr.contentRevision(id) == 1, "replace bumps the revision");
    check(changed.size() == 1 && changed[0].first == id && changed[0].second == 1,
          "replace fires content-changed with the bumped revision");
    check(mgr.isReferenced(id), "ticket SURVIVES replace (计数不动)");
    check(zeroed.empty(), "content change does NOT forge a zero-edge");
    check(invalidated.empty(),
          "content change does NOT forge an invalidation (对象一直在场)");
    check(mgr.fetchAsset(id) != nullptr, "the new object is in place");

    // ── notifyContentChanged:内存路径的显式宣告,同一条链。──
    mgr.notifyContentChanged(id);
    check(mgr.contentRevision(id) == 2, "notify bumps too");
    check(changed.size() == 2 && changed[1].second == 2, "notify broadcasts too");

    // ── replaceAsset 对未注册 id 拒绝(那是 registerAsset 的活)。──
    const asset_id_t stranger = mgr.generateUUID();
    check(!mgr.replaceAsset(makeSkeletonAssetWithId(stranger)),
          "replace refuses an unregistered id");
    check(changed.size() == 2, "refused replace broadcasts nothing");

    // ── removeAsset 也 ++revision(堵「删了又回来」的失配洞),但走的是
    //    invalidated 回调,不进 content_changed。──
    mgr.removeAsset(id);
    check(mgr.contentRevision(id) == 3, "removeAsset bumps the revision silently");
    check(changed.size() == 2,
          "removeAsset does NOT fire content-changed (语义互不混用)");
    check(invalidated.size() == 1, "removeAsset fires invalidation instead");
    mgr.setBroadcast({});
}

//-----------------------------------------------------------------------------
int main()
{
    std::cout << "Asset broadcast-seam acceptance test\n"
              << "====================================\n";

    test_remove_asset_keeps_ledger();
    test_asset_ref_value_semantics();
    test_invalidation_broadcast();
    test_content_changed_broadcast();
    test_registered_broadcast();
    test_loaded_asset_installation();

    std::cout << "\n" << std::string(60, '=') << "\n"
              << "  Summary: " << g_pass << " passed, "
                                << g_fail << " failed\n"
              << std::string(60, '=') << "\n";
    return g_fail == 0 ? 0 : 1;
}
