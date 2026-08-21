#pragma once
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cassert>
#include <random>
#include <string_view>
#include <thread>

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
        explicit AssetManagerImpl(
            std::shared_ptr<const AssetCodecCatalog> codecs)
            : codecs_(std::move(codecs))
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
            // ⚠️ 本方法**不碰 refcounts_**,这是契约不是疏漏(设计稿裁决七):
            // 兴趣比对象活得久。资产对象死后,持票者(AssetRef)随后松手时计数
            // 照常流干、归零广播照常触发 —— 派生 GPU 副本靠那个边沿回收。
            // 在这里清账,还在持有的票释放时就等不到归零边沿,GPU 副本必漏。
            // MaterialPreviewHost 的临时材质闭环今天就依赖这个语义
            // (removeAsset 后等归零广播确认 resolver 松手再收尾)。

            auto asset_it = assets_.find(id);
            if (asset_it == assets_.end())
                return;   // 摘除不存在的 id:no-op(不广播)

            // (曾在此同步 fire onWillUnload 订阅者 —— 批E2 退役:公开订阅面
            //  全仓生产零调用,「对象要没了」的事实由下面的 invalidated 广播
            //  承接,消费者经总线订阅。)

            // 失效广播(裁决七;批E 回调缝化):removeAsset 的调用时机不受控
            // (构建器不一定开着)—— 组装层回调只做无锁 publish,消费者在帧泵
            // 的安全点收,旧「队列形」的理由由总线原样承接。
            if (broadcast_.on_invalidated)
                broadcast_.on_invalidated(id);

            // 顺带 ++revision(**不**推 content_changed —— 对象没了走的是上面
            // 的 invalidated 语义):堵「删了又重新注册,但缓存自持票的依赖
            // (材质的贴图票)不归零 → 条目不销毁 → 解封后交回旧句柄」的
            // 静默洞 —— 回来的那份即使同 id,revision 也必然失配。
            ++revisions_[id];

            // After the broadcast ran the asset pointer is still live; now
            // free it.
            assets_.erase(asset_it);
        }

        [[nodiscard]] bool hasData(const asset_id_t& id) const
        {
            if (!hasAsset(id))
                return false;

            return assets_.at(id)->hasData();
        }

        // W2c: drop the asset's CPU data back to a shell (keep it registered).
        // The already-empty guard lives in LuxAsset::unloadData.
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
            // 注册事实广播(驻留 T2):失效封印的解除从此有推式信号,
            // 消费端不再每帧 queryInfo 轮询"资产回来了没"。
            if (broadcast_.on_registered)
                broadcast_.on_registered(id);
            return true;
        }

        // (曾有 onLoaded/onWillUnload/unsubscribe 生命周期订阅面(A1)——
        //  批E2 退役:AssetHandle 时代遗物,生产使用面归零(消费者每帧
        //  fetchAssetAs 重查;「数据要没了」走 invalidated 广播)。)

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
    public:
        void setVfs(std::shared_ptr<const AssetVfs> vfs) { vfs_ = std::move(vfs); }
        [[nodiscard]] const std::shared_ptr<const AssetVfs>& vfs() const { return vfs_; }

        [[nodiscard]] const std::shared_ptr<const AssetCodecCatalog>&
        codecCatalogOwner() const noexcept
        {
            return codecs_;
        }

        // ── 引用计数（见 AssetManager.hpp 那一节的成因）────────────────────
        //
        // 线程契约:主线程独占(非原子是刻意的 —— 原子计数只会让「后台线程持票」
        // 编译通过并把违约藏起来)。断言只在 debug 生效(NDEBUG 下消失,CLAUDE.md
        // 的 assert 规则在案);它抓的是开发期的装配错误,不是实机不变量 ——
        // 实机上违约的症状是计数悄悄错,而正确的预防是 AssetRef 的持有纪律。
        void assertLedgerThread() const noexcept
        {
            assert(std::this_thread::get_id() == owner_thread_ &&
                   "asset refcount ledger touched off the main thread");
        }

        void retain(const asset_id_t& id)
        {
            assertLedgerThread();
            if (id.is_nil()) return;
            ++refcounts_[id];
        }

        std::uint32_t release(const asset_id_t& id)
        {
            assertLedgerThread();
            if (id.is_nil()) return 0;
            const auto it = refcounts_.find(id);
            if (it == refcounts_.end()) return 0;   // 没 retain 过 / 已经是 0
            if (--it->second == 0)
            {
                refcounts_.erase(it);
                // 广播(批E:回调缝):归零事实交组装层翻译成 AssetUnreferenced
                // 事件 —— 多消费者由总线的多订阅承接(单队列时代两个场景抢一条
                // 队列,抢不到的那个 GPU 副本静默泄漏)。
                if (broadcast_.on_unreferenced)
                    broadcast_.on_unreferenced(id);
                return 0;
            }
            return it->second;
        }

        [[nodiscard]] bool isReferenced(const asset_id_t& id) const noexcept
        {
            return refcounts_.contains(id);
        }

        void setBroadcast(AssetManager::BroadcastCallbacks callbacks)
        {
            assertLedgerThread();
            broadcast_ = std::move(callbacks);
        }

        [[nodiscard]] std::uint32_t contentRevision(const asset_id_t& id) const
        {
            const auto it = revisions_.find(id);
            return it == revisions_.end() ? 0u : it->second;
        }

        /// 内容变更:++revision 并向每个消费者广播。**只挂作者语义出口**
        /// (见 revisions_ 的注释);对象必须在场 —— 对象没了那是 removeAsset
        /// 的 invalidated 语义,别混。
        void bumpContentRevision(const asset_id_t& id)
        {
            assertLedgerThread();
            if (id.is_nil()) return;
            const std::uint32_t rev = ++revisions_[id];
            if (broadcast_.on_content_changed)
                broadcast_.on_content_changed(id, rev);
        }

        /// 同 id 换对象(文件监视/重导入的落点):旧对象析构,新对象顶入,
        /// bump + content_changed 广播。**不推 invalidated、不动 refcounts_**
        /// —— 对象一直在场,票照常有效,派生副本按 revision 失配重建即可,
        /// 不触发宿主 fallback。id 未注册返回 false(那是 registerAsset 的活)。
        bool replaceAsset(std::unique_ptr<LuxAsset> asset)
        {
            assertLedgerThread();
            if (!asset || asset->info() == nullptr) return false;
            const asset_id_t id = asset->info()->id;
            auto it = assets_.find(id);
            if (it == assets_.end()) return false;

            it->second = std::move(asset);   // 旧对象在此析构

            bumpContentRevision(id);
            return true;
        }

    private:
        std::shared_ptr<const AssetVfs>                            vfs_;
        std::shared_ptr<const AssetCodecCatalog>                   codecs_;
        std::random_device                                         rd_;
        std::mt19937                                               generator_;
        uuids::uuid_random_generator*                              id_generator_;
        std::unordered_map<asset_id_t, std::unique_ptr<LuxAsset>>  assets_;

        /// 引用计数。**不含计数为 0 的条目** —— 归零即 erase，于是
        /// 「表里有没有」就是「还有没有人引用」，`isReferenced` 是一次哈希查找。
        std::unordered_map<asset_id_t, std::uint32_t> refcounts_;
        /// 广播回调缝(批E:三队列退役)。账本线程同步调,语义互不混用
        /// (unreferenced / invalidated / content_changed,见 AssetEvents.hpp);
        /// 组装层注入,把事实翻译成总线事件 —— 多消费者由总线承接。
        AssetManager::BroadcastCallbacks broadcast_{};

        /// 内容 revision(运行期,缺席 = 0)。**刻意不进 AssetInfo**:那个结构
        /// 按值嵌 AssetFileHeader,加字段 = 开 v3 + 写迁移 —— 纯运行期状态
        /// 不值得盘上格式代价。bump 的入口只有作者语义的出口(replaceAsset /
        /// notifyContentChanged)与 removeAsset(堵「删了又回来」的失配洞);
        /// 流送注入器「同样的字节装回」**不 bump**,否则每个驱逐-重载周期都
        /// 白重建一次 GPU 副本。
        std::unordered_map<asset_id_t, std::uint32_t> revisions_;

        /// 账本线程契约的锚点:构造 AssetManager 的线程即「主线程」。
        std::thread::id owner_thread_{std::this_thread::get_id()};
    };
}
