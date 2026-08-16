// ============================================================================
//  render_resource_manager_test — 驻留三件套 + 管道编排单测(J6-六/七修)。
//
//  纯状态表 + 全局服务(假域子服务)+ RenderResourceOps 管道(真
//  AsyncRuntime IO/CPU pools)。钉住:命中同步直通/缺数据异步终态(垃圾
//  VFS)/去重=行状态/终态复述/封印解封/1→0 双道复查/在途亡句柄自毁/
//  依赖级联+票据/RAII 等待退订/力扫;三条完成路各有归属。
//  §11(T6R2)接上每世界观察胶水(ResidencySubsystem + 真 EnTT
//  registry):折入存量/送达写回/防闪换代/网格材质合取/nil 语义/
//  M_Missing 换装/失效摘重请/离场 —— 接线 lambda 即 T11 宿主参考形状。
// ============================================================================

#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceOps.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>

#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/AssetVfs.hpp>
#include <lux/engine/resource/asset/SkeletonAsset.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using lux::asset::asset_id_t;
using lux::asset::AssetManager;
using lux::ecs::EResourceDomain;
using lux::exec::AsyncScope;
using lux::runtime::testing::AssetAsyncTestServices;
using lux::runtime::IRenderResourceSubservice;
using lux::runtime::RenderResourceService;
using lux::runtime::RenderResourceStateManager;
namespace ops = lux::runtime::render_resource;

static int g_fail = 0;
static void check(bool c, const char* what)
{
    std::printf("%s %s\n", c ? "[ ok ]" : "[FAIL]", what);
    if (!c) ++g_fail;
}

static std::unique_ptr<lux::asset::SkeletonAsset> makeAsset(asset_id_t id)
{
    auto info  = std::make_unique<lux::asset::AssetInfo>();
    info->id   = id;
    info->type = lux::asset::SkeletonAsset::asset_type;
    auto data  = std::make_unique<lux::rdesc::Skeleton>();
    lux::rdesc::Bone_t bone;
    bone.name = "root"; bone.parent_index = -1;
    bone.bind_local = Eigen::Affine3f::Identity();
    bone.inv_bind_world = Eigen::Affine3f::Identity();
    data->bones.push_back(std::move(bone));
    auto a = std::make_unique<lux::asset::SkeletonAsset>(std::move(info));
    a->setData(std::move(data));
    return a;
}

namespace
{
    /// 假域子服务:submit 暂存回执(测试手动触发 = RPC 回执到达)。
    /// 域可配 —— 胶水段(§11)另挂 MESH/MATERIAL 两份。
    struct FakeTextureSub final : IRenderResourceSubservice
    {
        explicit FakeTextureSub(
            EResourceDomain d = EResourceDomain::TEXTURE) noexcept
            : dom(d)
        {}

        EResourceDomain            dom;
        std::vector<asset_id_t>    submits;
        std::deque<SubmitDone>     pending;
        std::vector<std::uint64_t> destroyed;
        bool                       owner_controls_quiescent{true};
        /// 依赖申报覆写(§12 依赖门用;空 = 无依赖)。
        std::function<std::vector<lux::runtime::ResourceDep>(const asset_id_t&)>
            deps_fn;

        lux::ecs::EResourceDomain domain() const override { return dom; }
        std::vector<lux::runtime::ResourceDep>
        dependencies(const asset_id_t& id) const override
        {
            return deps_fn ? deps_fn(id)
                           : std::vector<lux::runtime::ResourceDep>{};
        }
        void submit(const asset_id_t& id, SubmitDone done) override
        {
            submits.push_back(id);
            pending.push_back(std::move(done));
        }
        void destroy(std::uint64_t bits) noexcept override
        { destroyed.push_back(bits); }
        [[nodiscard]] std::size_t pendingReplies() const noexcept override
        { return pending.size(); }
        [[nodiscard]] bool ownerControlsQuiescent() const noexcept override
        { return pending.empty() && owner_controls_quiescent; }
        void abandonPendingReplies() noexcept override
        { pending.clear(); }
        void settleNext(std::uint64_t bits, std::string_view fail = {})
        {
            auto done = std::move(pending.front());
            pending.pop_front();
            done(bits, fail);
        }
    };

    /// 胶水段(§11)的作者组件(存盘形状的最小替身)。
    struct TestSprite { asset_id_t texture{}; };
    struct TestMesh   { asset_id_t mesh{};  asset_id_t mat{}; };

    /// 坏字节 provider:缺数据路走真实池上加载 → 解码终态失败。
    class GarbageProvider final : public lux::asset::IAssetProvider
    {
    public:
        bool contains(const asset_id_t&) const override { return true; }
        lux::cxx::expected<lux::asset::AssetBlob, lux::asset::EAssetError>
        open(const asset_id_t&) const override
        {
            auto bytes = std::shared_ptr<std::byte[]>(new std::byte[8]);
            std::memset(bytes.get(), 0xAB, 8);
            return lux::asset::AssetBlob::fromSharedArray(
                std::move(bytes), 8u);
        }
        std::optional<asset_id_t> resolve(std::string_view) const override
        { return std::nullopt; }
        std::optional<std::string> pathOf(const asset_id_t&) const override
        { return std::nullopt; }
        void enumerate(const std::function<void(const lux::asset::ProviderEntry&)>&)
            const override {}
    };
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AssetManager               mgr{lux::asset::runtimeAssetCodecCatalog()};
    AssetAsyncTestServices     async(mgr);
    RenderResourceService      service;
    RenderResourceStateManager table;
    AsyncScope                 tasks(async.runtime());

    auto* sub = new FakeTextureSub;
    service.addSubservice(std::unique_ptr<IRenderResourceSubservice>(sub));
    auto* mesh_sub = new FakeTextureSub(EResourceDomain::MESH);
    auto* mat_sub  = new FakeTextureSub(EResourceDomain::MATERIAL);
    service.addSubservice(std::unique_ptr<IRenderResourceSubservice>(mesh_sub));
    service.addSubservice(std::unique_ptr<IRenderResourceSubservice>(mat_sub));

    // A resident owner carries a domain-bound release capability rather than
    // a raw service pointer. The service can prove terminal quiescence, while
    // lease moves still transfer one exact destroy obligation.
    {
        RenderResourceService capability_service;
        auto* capability_sub = new FakeTextureSub;
        capability_service.addSubservice(
            std::unique_ptr<IRenderResourceSubservice>(capability_sub)
        );
        check(capability_service.releaseQuiescent(),
              "P0A2-a: empty service release capability is quiescent");

        auto missing = capability_service.releaseEndpoint(
            EResourceDomain::MESH
        );
        check(!missing
                  && missing.error()
                         == lux::runtime::EResidentResourceAdoptError::
                                DomainUnavailable,
              "P0A2-b: endpoint acquisition reports a missing domain");

        auto endpoint_result = capability_service.releaseEndpoint(
            EResourceDomain::TEXTURE
        );
        check(static_cast<bool>(endpoint_result),
              "P0A2-c: installed domain yields a release capability");
        auto endpoint = std::move(*endpoint_result);
        check(!capability_service.releaseQuiescent(),
              "P0A2-d: an RPC-held release capability blocks terminal close");

        constexpr std::uint64_t kCapabilityBits = (0xA2ull << 32) | 1ull;
        {
            auto adopted = endpoint.adopt(kCapabilityBits);
            check(static_cast<bool>(adopted),
                  "P0A2-e: capability adopts a non-null GPU owner");
            auto lease = std::move(*adopted);
            auto moved = std::move(lease);
            check(!lease && moved,
                  "P0A2-f: lease move transfers the release obligation");
            endpoint = {};
            check(!capability_service.releaseQuiescent(),
                  "P0A2-g: live lease alone keeps release state non-quiescent");
        }
        check(capability_service.releaseQuiescent()
                  && capability_sub->destroyed.size() == 1
                  && capability_sub->destroyed.front() == kCapabilityBits,
              "P0A2-h: final lease destruction releases exactly once");
    }

    int fail_sink = 0;
    ops::Context ctx{table, service, mgr, async.client(), tasks, async.runtime(),
                     [&](const lux::ecs::RenderResourceFailed&) { ++fail_sink; },
                     {}};

    int ok_count = 0, fail_count = 0;
    auto counterWait = [&](const asset_id_t& id)
    {
        return service.await(id,
            [&](std::uint64_t bits, const lux::ecs::ResourceFailure* f)
            { if (f || bits == 0) ++fail_count; else ++ok_count; });
    };
    std::vector<RenderResourceService::WaitTicket> keep;
    auto pumpUntil = [&](auto cond)
    {
        for (int i = 0; i < 1'000'000 && !cond(); ++i)
        { async.drainMainThreadCompletions(); std::this_thread::yield(); }
    };

    using EState = RenderResourceStateManager::EState;
    constexpr std::uint64_t kBits = (0x2Aull << 32) | 7ull;

    // ── 1. 命中路:数据在账 → 管道同步直通到上传段;回执 READY 扇出 ────
    const auto id = mgr.generateUUID();
    mgr.registerAsset(makeAsset(id));
    keep.push_back(counterWait(id));
    ops::ensure(ctx, id, EResourceDomain::TEXTURE);
    keep.push_back(counterWait(id));
    ops::ensure(ctx, id, EResourceDomain::TEXTURE);   // UPLOADING:去重
    check(sub->submits.size() == 1, "1a: 命中同步直通上传段且去重=行状态");
    sub->settleNext(kBits);
    check(ok_count == 2, "1b: 回执 READY,两个等待各送达一次");
    check(table.find(id)->state == EState::READY
              && table.find(id)->resident.bits() == kBits,
          "1c: 表行 READY 且句柄正确");
    keep.push_back(counterWait(id));
    ops::ensure(ctx, id, EResourceDomain::TEXTURE);
    check(ok_count == 3 && sub->submits.size() == 1,
          "1d: 就绪后请求立即送达不重上传");

    // ── 2. 缺数据路:垃圾 VFS → 池上真实加载 → 解码终态 → upon_error ───
    {
        auto vfs = std::make_shared<lux::asset::AssetVfs>();
        (void)vfs->mount({.root = "/Game",
                          .provider = std::make_shared<GarbageProvider>(),
                          .priority = 0});
        mgr.setVfs(vfs);
        const auto miss = mgr.generateUUID();
        keep.push_back(counterWait(miss));
        ops::ensure(ctx, miss, EResourceDomain::TEXTURE);
        check(table.find(miss)->state == EState::LOADING, "2a: 缺数据置读取中");
        pumpUntil([&] { return table.find(miss)->state == EState::FAILED; });
        check(table.find(miss)->state == EState::FAILED && fail_count == 1,
              "2b: 解码终态经错误道落 FAILED 并送达");
        check(fail_sink >= 1, "2c: 失败观察触发");
        mgr.setVfs(nullptr);
    }

    // ── 3. 终态复述 + 无 VFS 缺数据 = 异步但响亮终态 ───────────────────
    {
        const auto miss2 = mgr.generateUUID();
        keep.push_back(counterWait(miss2));
        ops::ensure(ctx, miss2, EResourceDomain::TEXTURE);
        pumpUntil([&] { return table.find(miss2)->state == EState::FAILED; });
        check(fail_count == 2 && table.find(miss2)->state == EState::FAILED,
              "3a: 无 VFS 缺数据 → 结构化异步终态(不静默挂死)");
        keep.push_back(counterWait(miss2));
        ops::ensure(ctx, miss2, EResourceDomain::TEXTURE);
        check(fail_count == 3, "3b: 终态复述,不重走管道");
    }

    // ── 4. 上传失败 → 错误道 FAILED ────────────────────────────────────
    const auto id2 = mgr.generateUUID();
    mgr.registerAsset(makeAsset(id2));
    keep.push_back(counterWait(id2));
    ops::ensure(ctx, id2, EResourceDomain::TEXTURE);
    sub->settleNext(0, "boom");
    check(fail_count == 4 && table.find(id2)->state == EState::FAILED,
          "4a: 上传失败走错误道落 FAILED");

    // ── 5. 1→0 复查 + 归零销毁经域 destroy ─────────────────────────────
    {
        auto ticket = mgr.acquire(id);
        ops::onUnreferenced(ctx, id);
        check(sub->destroyed.empty(), "5a: isReferenced 复查挡陈旧归零");
    }
    ops::onUnreferenced(ctx, id);
    check(sub->destroyed.size() == 1 && sub->destroyed[0] == kBits,
          "5b: 归零销毁经域子服务 destroy");

    // ── 6. 封印 → 同步终败;解封 → 失效扇出 + 干净重走 ─────────────────
    int inv_hits = 0;
    auto inv_watch = service.watchInvalidation(
        [&](const std::vector<asset_id_t>&) { ++inv_hits; });
    const auto id3 = mgr.generateUUID();
    ops::onInvalidated(ctx, id3);
    keep.push_back(counterWait(id3));
    ops::ensure(ctx, id3, EResourceDomain::TEXTURE);
    check(fail_count == 5, "6a: 封印终败送达");
    ops::onAssetRegistered(ctx, id3);
    check(inv_hits == 1, "6b: 解封经失效扇出");

    // Failure delivery snapshots its payload, and invalidation subscriptions
    // tolerate self/peer removal without keeping an invalid vector iterator.
    {
        RenderResourceService reentrant_service;
        const auto reentrant_id = mgr.generateUUID();
        lux::ecs::ResourceFailure mutable_fail{
            lux::ecs::EResourceStage::UPLOAD,
            lux::ecs::EFailureClass::TERMINAL,
            "stable failure"
        };
        std::string second_reason;
        auto first_wait = reentrant_service.await(
            reentrant_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            { mutable_fail.reason = "mutated by first waiter"; }
        );
        auto second_wait = reentrant_service.await(
            reentrant_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure* failure)
            { second_reason = failure ? failure->reason : std::string{}; }
        );
        reentrant_service.notifyFailed(reentrant_id, mutable_fail);
        check(second_reason == "stable failure",
              "6c: 失败扇出使用稳定快照,前一 waiter 重入不污染后一 waiter");

        std::optional<RenderResourceService::WaitTicket> first_cancel_wait;
        std::optional<RenderResourceService::WaitTicket> peer_cancel_wait;
        int first_wait_hits = 0;
        int peer_wait_hits  = 0;
        first_cancel_wait.emplace(reentrant_service.await(
            reentrant_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            {
                ++first_wait_hits;
                first_cancel_wait.reset();
                peer_cancel_wait.reset();
            }
        ));
        peer_cancel_wait.emplace(reentrant_service.await(
            reentrant_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            { ++peer_wait_hits; }
        ));
        reentrant_service.notifyReady(reentrant_id, 1);
        check(first_wait_hits == 1 && peer_wait_hits == 0,
              "6d: wait 回调可自退订/互退订,局部批次仍跳过已取消 peer");

        std::optional<RenderResourceService::WaitTicket> first_fail_wait;
        std::optional<RenderResourceService::WaitTicket> peer_fail_wait;
        int first_fail_hits = 0;
        int peer_fail_hits  = 0;
        first_fail_wait.emplace(reentrant_service.await(
            reentrant_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            {
                ++first_fail_hits;
                peer_fail_wait.reset();
            }
        ));
        peer_fail_wait.emplace(reentrant_service.await(
            reentrant_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            { ++peer_fail_hits; }
        ));
        reentrant_service.notifyFailed(reentrant_id, mutable_fail);
        check(first_fail_hits == 1 && peer_fail_hits == 0,
              "6e: failed wait 批次同样跳过被前一回调取消的 peer");

        std::optional<RenderResourceService::InvalidationTicket> first_inv;
        std::optional<RenderResourceService::InvalidationTicket> peer_inv;
        std::optional<RenderResourceService::InvalidationTicket> added_inv;
        int first_inv_hits = 0;
        int peer_inv_hits  = 0;
        int added_inv_hits = 0;
        first_inv.emplace(reentrant_service.watchInvalidation(
            [&](const std::vector<asset_id_t>&)
            {
                ++first_inv_hits;
                first_inv.reset();
                peer_inv.reset();
                added_inv.emplace(reentrant_service.watchInvalidation(
                    [&](const std::vector<asset_id_t>&)
                    { ++added_inv_hits; }
                ));
            }
        ));
        peer_inv.emplace(reentrant_service.watchInvalidation(
            [&](const std::vector<asset_id_t>&)
            { ++peer_inv_hits; }
        ));
        reentrant_service.notifyInvalidated({reentrant_id});
        reentrant_service.notifyInvalidated({reentrant_id});
        check(first_inv_hits == 1 && peer_inv_hits == 0
                  && added_inv_hits == 1,
              "6f: 失效回调可自退订/互退订,本轮新增者留到下一轮");
    }

    // Tickets use a weak service-lifetime generation. They may outlive the
    // service, be moved, and reset/destruct afterwards without dereferencing
    // the dead service.
    {
        int late_wait_hits = 0;
        int late_inv_hits  = 0;
        {
            std::optional<RenderResourceService::WaitTicket> late_wait;
            std::optional<RenderResourceService::InvalidationTicket> late_inv;
            std::optional<RenderResourceService::WaitTicket> destruct_wait;
            std::optional<RenderResourceService::InvalidationTicket> destruct_inv;
            std::optional<RenderResourceService::WaitTicket>
                callback_owner_wait;
            {
                RenderResourceService short_lived_service;
                const auto short_lived_id = mgr.generateUUID();
                const auto make_wait = [&]
                {
                    return short_lived_service.await(
                        short_lived_id,
                        [&](std::uint64_t,
                            const lux::ecs::ResourceFailure*)
                        { ++late_wait_hits; }
                    );
                };
                const auto make_invalidation = [&]
                {
                    return short_lived_service.watchInvalidation(
                        [&](const std::vector<asset_id_t>&)
                        { ++late_inv_hits; }
                    );
                };
                late_wait.emplace(make_wait());
                late_inv.emplace(make_invalidation());
                destruct_wait.emplace(make_wait());
                destruct_inv.emplace(make_invalidation());
                auto nested_wait = make_wait();
                callback_owner_wait.emplace(short_lived_service.await(
                    mgr.generateUUID(),
                    [owned = std::move(nested_wait)](
                        std::uint64_t,
                        const lux::ecs::ResourceFailure*) mutable noexcept
                    { owned.reset(); }
                ));
                check(late_wait->active(),
                      "P0A-a: 存活 service 的 wait ticket 有效");
            }
            check(!late_wait->active(),
                  "P0A-b: service 先析构使 wait ticket 失效");
            auto moved_late_wait = std::move(*late_wait);
            auto moved_late_inv  = std::move(*late_inv);
            RenderResourceService::WaitTicket moved_assigned_late_wait;
            RenderResourceService::InvalidationTicket moved_assigned_late_inv;
            moved_assigned_late_wait = std::move(moved_late_wait);
            moved_assigned_late_inv  = std::move(moved_late_inv);
            late_wait->reset();
            late_inv->reset();
            moved_assigned_late_wait.reset();
            moved_assigned_late_inv.reset();
            check(late_wait_hits == 0 && late_inv_hits == 0,
                  "P0A-c: service 后的 ticket move/reset 不触发回调");
            // destruct_wait/destruct_inv deliberately retain expired weak
            // controls until this inner scope ends.
        }
        check(late_wait_hits == 0 && late_inv_hits == 0,
              "P0A-d: service 后的非空 ticket 析构安全 no-op");
    }

    // Reusing the exact service address must not let an old token erase a
    // subscription from the replacement generation.
    {
        alignas(RenderResourceService)
            std::byte storage[sizeof(RenderResourceService)];
        const auto reused_id = mgr.generateUUID();
        int old_hits = 0;
        int replacement_hits = 0;

        auto* first = std::construct_at(
            reinterpret_cast<RenderResourceService*>(storage)
        );
        auto old_ticket = first->await(
            reused_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            { ++old_hits; }
        );
        std::destroy_at(first);

        auto* replacement = std::construct_at(
            reinterpret_cast<RenderResourceService*>(storage)
        );
        auto replacement_ticket = replacement->await(
            reused_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            { ++replacement_hits; }
        );
        old_ticket.reset();
        check(replacement->pendingWaits(reused_id) == 1,
              "P0A-e: 旧 generation ticket 不退订同地址新 service");
        replacement->notifyReady(reused_id, 1);
        check(old_hits == 0 && replacement_hits == 1,
              "P0A-f: 地址复用后只触发新 generation 订阅");
        replacement_ticket.reset();
        std::destroy_at(replacement);
    }

    // Move construction transfers only the registration; move assignment
    // first retires its old registration, for both ticket families.
    {
        RenderResourceService move_service;
        const auto move_id = mgr.generateUUID();
        int retired_wait_hits = 0;
        int kept_wait_hits    = 0;
        auto retired_wait = move_service.await(
            move_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            { ++retired_wait_hits; }
        );
        auto wait_source = move_service.await(
            move_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            { ++kept_wait_hits; }
        );
        auto moved_wait = std::move(wait_source);
        retired_wait = std::move(moved_wait);
        wait_source.reset();
        moved_wait.reset();
        move_service.notifyReady(move_id, 1);
        check(retired_wait_hits == 0 && kept_wait_hits == 1
                  && !retired_wait.active(),
              "P0A-g: wait ticket move ctor/assign 保留新订阅并退订旧订阅");

        int retired_inv_hits = 0;
        int kept_inv_hits    = 0;
        auto retired_inv = move_service.watchInvalidation(
            [&](const std::vector<asset_id_t>&) { ++retired_inv_hits; }
        );
        auto inv_source = move_service.watchInvalidation(
            [&](const std::vector<asset_id_t>&) { ++kept_inv_hits; }
        );
        auto moved_inv = std::move(inv_source);
        retired_inv = std::move(moved_inv);
        inv_source.reset();
        moved_inv.reset();
        move_service.notifyInvalidated({move_id});
        check(retired_inv_hits == 0 && kept_inv_hits == 1,
              "P0A-h: invalidation ticket move ctor/assign 保留新订阅并退订旧订阅");
    }

    // A callback may release the last service owner. The lexical dispatch
    // keeps only the invalidatable control alive, stops the remaining batch,
    // and never writes through the destroyed service object.
    {
        auto self_destroy_service =
            std::make_unique<RenderResourceService>();
        const auto self_destroy_id = mgr.generateUUID();
        std::optional<RenderResourceService::WaitTicket> destroy_wait;
        std::optional<RenderResourceService::WaitTicket> skipped_wait;
        int skipped_hits = 0;
        destroy_wait.emplace(self_destroy_service->await(
            self_destroy_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            { self_destroy_service.reset(); }
        ));
        skipped_wait.emplace(self_destroy_service->await(
            self_destroy_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            { ++skipped_hits; }
        ));
        auto* dispatch_entry = self_destroy_service.get();
        dispatch_entry->notifyReady(self_destroy_id, 1);
        check(!self_destroy_service && skipped_hits == 0
                  && !destroy_wait->active() && !skipped_wait->active(),
              "P0A-i: wait 回调销毁 service 后终止局部批次且控制块安全退栈");
        destroy_wait.reset();
        skipped_wait.reset();
    }

    // Invalidation fan-out follows the same lifetime rule: once a callback
    // releases the service, peers from the snapshotted batch must not run.
    {
        auto self_destroy_service =
            std::make_unique<RenderResourceService>();
        std::optional<RenderResourceService::InvalidationTicket> destroy_inv;
        std::optional<RenderResourceService::InvalidationTicket> skipped_inv;
        int skipped_hits = 0;
        destroy_inv.emplace(self_destroy_service->watchInvalidation(
            [&](const std::vector<asset_id_t>&)
            { self_destroy_service.reset(); }
        ));
        skipped_inv.emplace(self_destroy_service->watchInvalidation(
            [&](const std::vector<asset_id_t>&)
            { ++skipped_hits; }
        ));
        auto* dispatch_entry = self_destroy_service.get();
        dispatch_entry->notifyInvalidated({mgr.generateUUID()});
        check(!self_destroy_service && skipped_hits == 0,
              "P0A-j: invalidation 回调销毁 service 后终止局部批次");
        destroy_inv.reset();
        skipped_inv.reset();
    }

    mgr.registerAsset(makeAsset(id3));
    keep.push_back(counterWait(id3));
    ops::ensure(ctx, id3, EResourceDomain::TEXTURE);
    check(sub->submits.size() == 3, "6g: 解封后重请求直接上传");

    // ── 7. 在途亡:内容变更销毁 UPLOADING 行 → 迟到回执自毁句柄 ────────
    ops::onContentChanged(ctx, id3);
    sub->settleNext(kBits);   // 迟到回执
    check(!sub->destroyed.empty() && sub->destroyed.back() == kBits,
          "7a: 行在途亡 → 到达句柄当场自毁");

    // 7b: 更强的 ABA 交错 —— 两个 wiring Context 共享同一状态表。
    // 擦旧行后由另一个 Context 发起同 id 新链；attempt identity 必须
    // 来自 table SSOT，旧成功仍只能释放自己，不能穿进新行。
    ops::Context sibling_ctx{
        table, service, mgr, async.client(), tasks, async.runtime(), {}, {}};
    const auto aba_id = mgr.generateUUID();
    mgr.registerAsset(makeAsset(aba_id));
    ops::ensure(ctx, aba_id, EResourceDomain::TEXTURE);
    const auto old_serial = table.find(aba_id)->operation_serial;
    ops::onContentChanged(ctx, aba_id);
    ops::ensure(sibling_ctx, aba_id, EResourceDomain::TEXTURE);
    const auto new_serial = table.find(aba_id)->operation_serial;
    constexpr std::uint64_t kOldBits = (0x71ull << 32) | 1ull;
    constexpr std::uint64_t kNewBits = (0x72ull << 32) | 1ull;
    sub->settleNext(kOldBits);
    check(old_serial != new_serial
              && table.find(aba_id)->state == EState::UPLOADING
              && !table.find(aba_id)->resident
              && !sub->destroyed.empty()
              && sub->destroyed.back() == kOldBits,
          "7b: 旧 attempt 成功不能穿透到同 id 新 UPLOADING 行");
    sub->settleNext(kNewBits);
    check(table.find(aba_id)->state == EState::READY
              && table.find(aba_id)->resident.bits() == kNewBits,
          "7c: 新 attempt 只接纳自己的句柄");

    // ── 8. RAII 等待退订:句柄先亡触发静默跳过 ─────────────────────────
    const auto id5 = mgr.generateUUID();
    mgr.registerAsset(makeAsset(id5));
    {
        auto short_lived = counterWait(id5);
        check(service.pendingWaits(id5) == 1, "8a: 登记在册");
    }
    check(service.pendingWaits(id5) == 0, "8b: 句柄析构自动退订");
    const int ok_before = ok_count;
    ops::ensure(ctx, id5, EResourceDomain::TEXTURE);
    sub->settleNext((9ull << 32) | 1);
    check(ok_count == ok_before, "8c: 已退订者不被触发(场景先亡=静默)");

    // ── 9. 依赖级联:链式 + 环 + 票据代持 ──────────────────────────────
    auto readyRow = [&](const asset_id_t& rid, std::uint64_t bits)
    {
        mgr.registerAsset(makeAsset(rid));
        ops::ensure(ctx, rid, EResourceDomain::TEXTURE);
        sub->settleNext(bits);
    };
    const auto A = mgr.generateUUID(), B = mgr.generateUUID(),
               C = mgr.generateUUID();
    readyRow(A, (1ull << 32) | 1);
    readyRow(B, (2ull << 32) | 1);
    readyRow(C, (3ull << 32) | 1);
    ops::setDependencies(ctx, B, {A});
    ops::setDependencies(ctx, C, {B});
    check(mgr.isReferenced(A), "9a: 依赖声明 → 票据代持入表行");
    sub->destroyed.clear();
    ops::onContentChanged(ctx, A);
    check(sub->destroyed.size() == 3, "9b: 链式级联三行全销毁");
    check(!mgr.isReferenced(A), "9c: 行亡票归还(1→0 可触发)");
    const auto D = mgr.generateUUID(), E = mgr.generateUUID();
    readyRow(D, (4ull << 32) | 1);
    readyRow(E, (5ull << 32) | 1);
    ops::setDependencies(ctx, D, {E});
    ops::setDependencies(ctx, E, {D});
    sub->destroyed.clear();
    ops::onContentChanged(ctx, D);
    check(sub->destroyed.size() == 2, "9d: 依赖环各销毁一次(防环终止)");

    // ── 10. teardown 力扫(有票不豁免;不依赖事件)─────────────────────
    const auto id6 = mgr.generateUUID();
    readyRow(id6, (6ull << 32) | 1);
    sub->destroyed.clear();
    {
        auto pin = mgr.acquire(id6);
        ops::teardown(ctx);
        // 在场就绪行 = aba_id(§7)+id5(§8)+id6;力扫全收。
        check(sub->destroyed.size() == 3, "10a: 力扫销毁全部在场句柄");
    }
    check(!ops::hasInflight(ctx) && table.size() == 0, "10b: 力扫后表空无在途");

    // A READY row itself is the unique physical GPU owner. Omitting explicit
    // teardown must still return the handle when the table leaves scope.
    const auto raii_id = mgr.generateUUID();
    mgr.registerAsset(makeAsset(raii_id));
    const auto destroyed_before_raii = sub->destroyed.size();
    {
        RenderResourceStateManager owner_table;
        ops::Context owner_ctx{
            owner_table,
            service,
            mgr,
            async.client(),
            tasks,
            async.runtime(),
            {},
            {}
        };
        ops::ensure(owner_ctx, raii_id, EResourceDomain::TEXTURE);
        sub->settleNext((7ull << 32) | 1);
        check(owner_table.find(raii_id)->state == EState::READY
                  && static_cast<bool>(owner_table.find(raii_id)->resident),
              "10c: READY row holds a move-only resident owner");
    }
    check(sub->destroyed.size() == destroyed_before_raii + 1,
          "10d: table scope exit releases READY handle without teardown");

    // ── 11. 每世界观察胶水(T6R2):真三件套接线的端到端 ─────────────────
    //
    // 本段的接线 lambda 就是 T11 宿主装配的参考形状:request→ops::ensure,
    // await→service.await(票据包成不透明 RAII),watch→watchInvalidation。
    {
        lux::meta::EntityRegistry reg;
        lux::ecs::ResidencySubsystem glue(mgr);

        auto wrapTicket = [](auto&& t)
        {
            using T = std::decay_t<decltype(t)>;
            return lux::ecs::ResidencyCallbacks::Ticket(
                [owned = T(std::move(t))]() mutable noexcept
                { owned.reset(); });
        };
        lux::ecs::ResidencyCallbacks cbs;
        cbs.request = [&](const asset_id_t& rid, EResourceDomain d)
        { ops::ensure(ctx, rid, d); };
        cbs.await = [&](const asset_id_t&                        rid,
                        lux::ecs::ResidencyCallbacks::DeliverFn d)
        {
            return wrapTicket(service.await(rid,
                [d = std::move(d)](std::uint64_t                  bits,
                                   const lux::ecs::ResourceFailure* f)
                { d(bits, f); }));
        };
        cbs.watch_invalidation =
            [&](std::function<void(const std::vector<asset_id_t>&)> f)
        {
            return wrapTicket(service.watchInvalidation(
                [f = std::move(f)](const std::vector<asset_id_t>& ids)
                { f(ids); }));
        };
        glue.setCallbacks(std::move(cbs));
        glue.resolveTextureOf<TestSprite, &TestSprite::texture>();
        glue.resolveMeshOf<TestMesh, &TestMesh::mesh, &TestMesh::mat>();

        // 11a: 折入存量 —— 组件先于 attach 挂上,信号没响过也不能漏。
        const auto T1 = mgr.generateUUID();
        mgr.registerAsset(makeAsset(T1));
        const auto e1 = reg.create();
        reg.emplace<TestSprite>(e1, TestSprite{T1});
        glue.attach(reg);
        const auto tex_subs = sub->submits.size();
        glue.drainResolvers(reg);
        check(sub->submits.size() == tex_subs + 1,
              "11a: 折入存量 → 边沿请求发出");
        sub->settleNext((0x30ull << 32) | 3);
        {
            auto* cc = reg.try_get<lux::ecs::TextureGpuCacheComponent>(e1);
            check(cc != nullptr && cc->handle.index == 0x30
                      && cc->source == T1,
                  "11b: 送达写回 cache 组件(解包正确)");
        }
        glue.drainResolvers(reg);
        check(sub->submits.size() == tex_subs + 1,
              "11c: 幂等 —— 同意图不重复请求");

        // 11d: patch 换图 —— 旧组件保持到新图到位(防闪),票据换代。
        const auto T2 = mgr.generateUUID();
        mgr.registerAsset(makeAsset(T2));
        reg.patch<TestSprite>(e1, [&](TestSprite& s) { s.texture = T2; });
        glue.drainResolvers(reg);
        {
            auto* cc = reg.try_get<lux::ecs::TextureGpuCacheComponent>(e1);
            check(cc != nullptr && cc->handle.index == 0x30,
                  "11d: 换图在途保持旧组件(不闪)");
        }
        sub->settleNext((0x31ull << 32) | 1);
        {
            auto* cc = reg.try_get<lux::ecs::TextureGpuCacheComponent>(e1);
            check(cc != nullptr && cc->handle.index == 0x31
                      && cc->source == T2,
                  "11e: 新图到位换写组件");
        }

        // 11f: 网格+材质合取 —— 两者都好组件才出现。
        const auto M1 = mgr.generateUUID(), MT1 = mgr.generateUUID();
        mgr.registerAsset(makeAsset(M1));
        mgr.registerAsset(makeAsset(MT1));
        const auto e2 = reg.create();
        reg.emplace<TestMesh>(e2, TestMesh{M1, MT1});
        glue.drainResolvers(reg);
        mesh_sub->settleNext((0x40ull << 32) | 1);
        check(!reg.all_of<lux::ecs::MeshGpuCacheComponent>(e2),
              "11f: 材质未好,合取不写");
        mat_sub->settleNext((0x41ull << 32) | 1);
        {
            auto* mc = reg.try_get<lux::ecs::MeshGpuCacheComponent>(e2);
            check(mc != nullptr && mc->mesh.index == 0x40
                      && mc->material.index == 0x41
                      && mc->material_source == MT1,
                  "11g: 两者就绪合取写入");
        }

        // 11h: 材质 nil = 作者没设(合法),网格就绪即写、材质空句柄。
        const auto M2 = mgr.generateUUID();
        mgr.registerAsset(makeAsset(M2));
        const auto e3 = reg.create();
        reg.emplace<TestMesh>(e3, TestMesh{M2, {}});
        glue.drainResolvers(reg);
        mesh_sub->settleNext((0x42ull << 32) | 1);
        {
            auto* mc = reg.try_get<lux::ecs::MeshGpuCacheComponent>(e3);
            check(mc != nullptr && mc->mesh.index == 0x42
                      && mc->material.isNull()
                      && mc->material_source.is_nil(),
                  "11h: 材质 nil ≠ 没好 —— 空句柄照写");
        }

        // 11i: 材质终败 → M_Missing 换装(实体变色不消失;原票保留可逆)。
        const auto missing_id = lux::asset::builtinMissingMaterialId();
        mgr.registerAsset(makeAsset(missing_id));   // 兜底本体可上传
        const auto M3  = mgr.generateUUID();
        const auto BAD = mgr.generateUUID();        // 不注册、无 VFS → 异步终败
        mgr.registerAsset(makeAsset(M3));
        const auto e4 = reg.create();
        reg.emplace<TestMesh>(e4, TestMesh{M3, BAD});
        glue.drainResolvers(reg);
        mesh_sub->settleNext((0x43ull << 32) | 1);
        pumpUntil([&] { return !mat_sub->pending.empty(); });
        mat_sub->settleNext((0x44ull << 32) | 1);   // 兜底材质回执
        {
            auto* mc = reg.try_get<lux::ecs::MeshGpuCacheComponent>(e4);
            check(mc != nullptr && mc->material.index == 0x44
                      && mc->material_source == missing_id,
                  "11i: 材质终败换装 M_Missing(source=兜底)");
        }

        // 11j: 内容变更 → 失效推送摘死句柄组件 → 自动重请求重送达。
        const auto tex_subs2 = sub->submits.size();
        ops::onContentChanged(ctx, T2);
        check(!reg.all_of<lux::ecs::TextureGpuCacheComponent>(e1),
              "11j: 失效推送当帧摘死句柄组件");
        glue.drainResolvers(reg);
        check(sub->submits.size() == tex_subs2 + 1,
              "11k: 失效后自动重请求(兴趣票未断)");
        sub->settleNext((0x32ull << 32) | 1);
        {
            auto* cc = reg.try_get<lux::ecs::TextureGpuCacheComponent>(e1);
            check(cc != nullptr && cc->handle.index == 0x32,
                  "11l: 重建句柄重新送达");
        }

        // 11m: 离场 —— 组件摘除,等待/票据随记录亡(RAII)。
        reg.remove<TestSprite>(e1);
        glue.drainResolvers(reg);
        check(!reg.all_of<lux::ecs::TextureGpuCacheComponent>(e1),
              "11m: 离场摘 cache 组件");

        glue.detach();
    }

    // ── 12. 依赖门(T9 使能件):发现→票据级联→结算门→环检 ──────────────
    {
        using lux::runtime::ResourceDep;
        const auto M = mgr.generateUUID(), T = mgr.generateUUID();
        mgr.registerAsset(makeAsset(M));
        mgr.registerAsset(makeAsset(T));
        mat_sub->deps_fn = [&](const asset_id_t& rid)
            -> std::vector<ResourceDep>
        {
            if (rid == M)
                return {{T, EResourceDomain::TEXTURE}};
            return {};
        };

        // 12a: 依赖未结算 → 材质停在门内(LOADING,不 submit);
        //      依赖行自动发起 + 票据代持(级联边随行)。
        const auto mat_subs0 = mat_sub->submits.size();
        keep.push_back(counterWait(M));
        ops::ensure(ctx, M, EResourceDomain::MATERIAL);
        check(mat_sub->submits.size() == mat_subs0
                  && table.find(M)->state == EState::LOADING,
              "12a: 依赖未结算,材质停在门内不上传");
        check(table.find(T) != nullptr
                  && table.find(T)->state == EState::UPLOADING
                  && mgr.isReferenced(T),
              "12b: 依赖自动发起且票据代持");

        // 12c: 依赖就绪 → 门开 → 材质 submit → READY。
        sub->settleNext((0x50ull << 32) | 1);
        check(mat_sub->submits.size() == mat_subs0 + 1,
              "12c: 依赖就绪门开,材质进入上传");
        mat_sub->settleNext((0x51ull << 32) | 1);
        check(table.find(M)->state == EState::READY,
              "12d: 材质随后 READY");

        // 12e: 级联仍通:依赖内容变更 → 材质行随波前销毁。
        mat_sub->destroyed.clear();
        ops::onContentChanged(ctx, T);
        check(mat_sub->destroyed.size() == 1
                  && mat_sub->destroyed[0] == ((0x51ull << 32) | 1),
              "12e: 依赖换代级联销毁材质行");

        // 12f: 依赖终败**不挡** submit(槽位策略在配方)。
        const auto M2 = mgr.generateUUID(), BAD = mgr.generateUUID();
        mgr.registerAsset(makeAsset(M2));   // BAD 不注册、无 VFS → 异步终败
        mat_sub->deps_fn = [&](const asset_id_t& rid)
            -> std::vector<ResourceDep>
        {
            if (rid == M2)
                return {{BAD, EResourceDomain::TEXTURE}};
            return {};
        };
        ops::ensure(ctx, M2, EResourceDomain::MATERIAL);
        pumpUntil([&] { return !mat_sub->pending.empty(); });
        check(!mat_sub->pending.empty()
                  && mat_sub->submits.back() == M2,
              "12f: 依赖终败不挡上传(坏槽策略在配方)");
        mat_sub->settleNext((0x52ull << 32) | 1);

        // 12g: 环检测:A↔B 互依 → 后进者终败(cyclic),先进者不死锁。
        const auto A2 = mgr.generateUUID(), B2 = mgr.generateUUID();
        mgr.registerAsset(makeAsset(A2));
        mgr.registerAsset(makeAsset(B2));
        mat_sub->deps_fn = [&](const asset_id_t& rid)
            -> std::vector<ResourceDep>
        {
            if (rid == A2) return {{B2, EResourceDomain::MATERIAL}};
            if (rid == B2) return {{A2, EResourceDomain::MATERIAL}};
            return {};
        };
        ops::ensure(ctx, A2, EResourceDomain::MATERIAL);
        check(table.find(B2)->state == EState::FAILED
                  && table.find(B2)->last_fail.reason.find("cyclic")
                         != std::string::npos,
              "12g: 环上后进者终败(cyclic),不死锁");
        check(!mat_sub->pending.empty()
                  && mat_sub->submits.back() == A2,
              "12h: 先进者照常进入上传(依赖失败不挡)");
        mat_sub->settleNext((0x53ull << 32) | 1);
        mat_sub->deps_fn = {};
    }

    // ── 13. asyncOp stop handoff:永不回调不再卡 owner scope close；
    //          stop 后迟到 GPU owner 由 router adopt + RAII 补偿 ──────
    {
        AssetManager stop_mgr{lux::asset::runtimeAssetCodecCatalog()};
        AssetAsyncTestServices stop_async(stop_mgr);
        RenderResourceService stop_service;
        RenderResourceStateManager stop_table;
        AsyncScope stop_tasks(stop_async.runtime());
        auto* stop_sub = new FakeTextureSub;
        stop_service.addSubservice(
            std::unique_ptr<IRenderResourceSubservice>(stop_sub)
        );
        ops::Context stop_ctx{
            stop_table,
            stop_service,
            stop_mgr,
            stop_async.client(),
            stop_tasks,
            stop_async.runtime(),
            {},
            {}
        };

        const auto stop_id = stop_mgr.generateUUID();
        stop_mgr.registerAsset(makeAsset(stop_id));
        ops::ensure(
            stop_ctx,
            stop_id,
            EResourceDomain::TEXTURE
        );
        check(stop_sub->pending.size() == 1
                  && stop_table.find(stop_id)->state == EState::UPLOADING
                  && !stop_service.releaseQuiescent(),
              "13a: fake upload is deliberately left without a callback");

        lux::exec::testing::closeScope(
            stop_tasks,
            stop_async.runtime()
        );
        check(stop_table.find(stop_id)->state == EState::UPLOADING,
              "13b: executor stop returns without publishing a false terminal");

        constexpr std::uint64_t kLateBits = (0x73ull << 32) | 1ull;
        stop_sub->settleNext(kLateBits);
        check(stop_sub->destroyed.size() == 1
                  && stop_sub->destroyed.front() == kLateBits
                  && stop_table.find(stop_id)->state == EState::UPLOADING
                  && stop_service.releaseQuiescent(),
              "13c: stopped sender reaps a late handle exactly once");
        ops::teardown(stop_ctx);
    }

    // 14. Protocol hardening: a malformed reply can carry both a handle and
    // an error. The handle is still transferred ownership and therefore must
    // be destroyed, while the row takes the failure terminal.
    {
        AssetManager malformed_mgr{lux::asset::runtimeAssetCodecCatalog()};
        AssetAsyncTestServices malformed_async(malformed_mgr);
        RenderResourceService malformed_service;
        RenderResourceStateManager malformed_table;
        AsyncScope malformed_tasks(malformed_async.runtime());
        auto* malformed_sub = new FakeTextureSub;
        malformed_service.addSubservice(
            std::unique_ptr<IRenderResourceSubservice>(malformed_sub)
        );
        ops::Context malformed_ctx{
            malformed_table,
            malformed_service,
            malformed_mgr,
            malformed_async.client(),
            malformed_tasks,
            malformed_async.runtime(),
            {},
            {}
        };

        const auto malformed_id = malformed_mgr.generateUUID();
        malformed_mgr.registerAsset(makeAsset(malformed_id));
        ops::ensure(
            malformed_ctx,
            malformed_id,
            EResourceDomain::TEXTURE
        );
        constexpr std::uint64_t kMalformedBits = (0x91ull << 32) | 3ull;
        malformed_sub->settleNext(kMalformedBits, "handle plus failure");
        check(malformed_table.find(malformed_id)->state == EState::FAILED
                  && malformed_sub->destroyed.size() == 1
                  && malformed_sub->destroyed.front() == kMalformedBits,
              "14: non-null failure reply compensates handle before failing");
        ops::teardown(malformed_ctx);
        lux::exec::testing::closeScope(
            malformed_tasks,
            malformed_async.runtime()
        );
    }

    // 15. The owner scope is the admission boundary. Once close starts, a
    // late residency request must become a loud terminal failure instead of
    // leaving a LOADING row whose sender can no longer be owned.
    {
        AssetManager closed_mgr{lux::asset::runtimeAssetCodecCatalog()};
        AssetAsyncTestServices closed_async(closed_mgr);
        RenderResourceService closed_service;
        RenderResourceStateManager closed_table;
        AsyncScope closed_tasks(closed_async.runtime());
        auto* closed_sub = new FakeTextureSub;
        closed_service.addSubservice(
            std::unique_ptr<IRenderResourceSubservice>(closed_sub)
        );
        ops::Context closed_ctx{
            closed_table,
            closed_service,
            closed_mgr,
            closed_async.client(),
            closed_tasks,
            closed_async.runtime(),
            {},
            {}
        };

        lux::exec::testing::closeScope(
            closed_tasks,
            closed_async.runtime()
        );
        const auto closed_id = closed_mgr.generateUUID();
        closed_mgr.registerAsset(makeAsset(closed_id));
        ops::ensure(
            closed_ctx,
            closed_id,
            EResourceDomain::TEXTURE
        );

        const auto* closed_row = closed_table.find(closed_id);
        check(closed_row != nullptr
                  && closed_row->state == EState::FAILED
                  && closed_row->last_fail.reason.find("scope is closed")
                         != std::string::npos
                  && closed_sub->pending.empty(),
              "15: closed owner scope rejects request with a terminal failure");
        ops::teardown(closed_ctx);
    }

    // 16. Close accounting is registration-driven, not a duplicated enum
    // list. Every installed subservice participates, and retryable reports do
    // not authorize the composition root to stop borrowed dependencies.
    {
        RenderResourceService aggregate_service;
        auto* aggregate_texture = new FakeTextureSub(
            EResourceDomain::TEXTURE
        );
        auto* aggregate_mesh = new FakeTextureSub(
            EResourceDomain::MESH
        );
        auto* aggregate_material = new FakeTextureSub(
            EResourceDomain::MATERIAL
        );
        aggregate_service.addSubservice(
            std::unique_ptr<IRenderResourceSubservice>(aggregate_texture)
        );
        aggregate_service.addSubservice(
            std::unique_ptr<IRenderResourceSubservice>(aggregate_mesh)
        );
        aggregate_service.addSubservice(
            std::unique_ptr<IRenderResourceSubservice>(aggregate_material)
        );

        const auto aggregate_id = mgr.generateUUID();
        const auto hold = [](std::uint64_t, std::string_view) noexcept {};
        (void)aggregate_service.submit(
            EResourceDomain::TEXTURE,
            aggregate_id,
            hold
        );
        (void)aggregate_service.submit(
            EResourceDomain::MESH,
            aggregate_id,
            hold
        );
        (void)aggregate_service.submit(
            EResourceDomain::MATERIAL,
            aggregate_id,
            hold
        );
        check(aggregate_service.pendingReplies() == 3,
              "16a: service aggregates every registered domain reaper");
        aggregate_service.abandonPendingReplies();
        check(aggregate_service.pendingReplies() == 0
                  && aggregate_texture->pending.empty()
                  && aggregate_mesh->pending.empty()
                  && aggregate_material->pending.empty(),
              "16b: service abandons every registered domain reaper");
        aggregate_material->owner_controls_quiescent = false;
        check(!aggregate_service.ownerControlsQuiescent(),
              "16c: one live domain owner control blocks terminal close proof");
        aggregate_material->owner_controls_quiescent = true;
        check(aggregate_service.ownerControlsQuiescent(),
              "16d: aggregate owner-control proof closes only after every domain");

        constexpr lux::runtime::ResidencyCloseReport in_progress{
            lux::runtime::EResidencyCloseStatus::CloseInProgress
        };
        static_assert(in_progress.retryable() && !in_progress.terminal());
    }

    // 17. Operation generation separates admission from continuation. Work
    // admitted before DRAINING may settle; new roots and external asset facts
    // are rejected until terminal INVALID, without a lock or per-row control.
    {
        AssetManager phase_mgr{lux::asset::runtimeAssetCodecCatalog()};
        AssetAsyncTestServices phase_async(phase_mgr);
        RenderResourceService phase_service;
        RenderResourceStateManager phase_table;
        AsyncScope phase_tasks(phase_async.runtime());
        auto* phase_sub = new FakeTextureSub;
        phase_service.addSubservice(
            std::unique_ptr<IRenderResourceSubservice>(phase_sub)
        );
        ops::Context phase_ctx{
            phase_table,
            phase_service,
            phase_mgr,
            phase_async.client(),
            phase_tasks,
            phase_async.runtime(),
            {},
            {}
        };

        const auto admitted_id = phase_mgr.generateUUID();
        phase_mgr.registerAsset(makeAsset(admitted_id));
        ops::ensure(phase_ctx, admitted_id, EResourceDomain::TEXTURE);
        check(phase_ctx.acceptsNewOperations()
                  && !phase_ctx.isDraining()
                  && phase_sub->pending.size() == 1,
              "17a: fresh generation accepts one upload attempt");

        phase_ctx.beginDraining();
        const auto rejected_id = phase_mgr.generateUUID();
        phase_mgr.registerAsset(makeAsset(rejected_id));
        ops::ensure(phase_ctx, rejected_id, EResourceDomain::TEXTURE);
        ops::onInvalidated(phase_ctx, rejected_id);
        check(!phase_ctx.acceptsNewOperations()
                  && phase_ctx.isDraining()
                  && phase_table.find(rejected_id) == nullptr
                  && phase_sub->pending.size() == 1,
              "17b: DRAINING rejects new roots and external asset facts");

        constexpr std::uint64_t kDrainingBits = (0xA3ull << 32) | 1ull;
        phase_sub->settleNext(kDrainingBits);
        check(phase_table.find(admitted_id)->state == EState::READY
                  && phase_table.find(admitted_id)->resident.bits()
                         == kDrainingBits,
              "17c: attempt admitted before DRAINING may still complete");

        lux::exec::testing::closeScope(
            phase_tasks,
            phase_async.runtime()
        );
        ops::teardown(phase_ctx);
        check(phase_ctx.operationQuiescent()
                  && phase_service.releaseQuiescent(),
              "17d: joined scope and table teardown prove both generations quiescent");
        phase_ctx.invalidateAfterJoin();
        check(!phase_ctx.acceptsNewOperations()
                  && !phase_ctx.isDraining(),
              "17e: terminal INVALID is permanent and rejects admission");
    }

    // 18. Attempt identity includes the asset revision as well as the serial.
    // This white-box interleaving leaves the serial unchanged and advances
    // only revision: the stale non-null owner must still be compensated.
    {
        AssetManager revision_mgr{lux::asset::runtimeAssetCodecCatalog()};
        AssetAsyncTestServices revision_async(revision_mgr);
        RenderResourceService revision_service;
        RenderResourceStateManager revision_table;
        AsyncScope revision_tasks(revision_async.runtime());
        auto* revision_sub = new FakeTextureSub;
        revision_service.addSubservice(
            std::unique_ptr<IRenderResourceSubservice>(revision_sub)
        );
        ops::Context revision_ctx{
            revision_table,
            revision_service,
            revision_mgr,
            revision_async.client(),
            revision_tasks,
            revision_async.runtime(),
            {},
            {}
        };

        const auto revision_id = revision_mgr.generateUUID();
        revision_mgr.registerAsset(makeAsset(revision_id));
        ops::ensure(revision_ctx, revision_id, EResourceDomain::TEXTURE);
        auto* revised_row = revision_table.find(revision_id);
        const auto unchanged_serial = revised_row->operation_serial;
        ++revised_row->revision;

        constexpr std::uint64_t kStaleRevisionBits =
            (0xA4ull << 32) | 1ull;
        revision_sub->settleNext(kStaleRevisionBits);
        check(revised_row->operation_serial == unchanged_serial
                  && revised_row->state == EState::UPLOADING
                  && !revised_row->resident
                  && revision_sub->destroyed.size() == 1
                  && revision_sub->destroyed.front()
                         == kStaleRevisionBits,
              "18: stale revision success cannot enter a same-serial row");

        lux::exec::testing::closeScope(
            revision_tasks,
            revision_async.runtime()
        );
        ops::teardown(revision_ctx);
        revision_ctx.beginDraining();
        check(revision_ctx.operationQuiescent()
                  && revision_service.releaseQuiescent(),
              "18b: stale owner compensation leaves terminal controls quiescent");
        revision_ctx.invalidateAfterJoin();
    }

    lux::exec::testing::closeScope(tasks, async.runtime());
    std::printf(g_fail == 0 ? "\nALL CHECKS PASSED\n" : "\n%d CHECK(S) FAILED\n",
                g_fail);
    return g_fail == 0 ? 0 : 1;
}
