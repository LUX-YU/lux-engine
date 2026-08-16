#pragma once
/**
 * @file ResidencySubsystem.hpp — 每世界驻留观察胶水(驻留 T6R2,J6-六修)。
 *
 * 三件套(纯状态表 + 全局服务 + 引擎编排管道)的 **ecs 侧末端**:观察作者
 * 组件的资产字段,向引擎发「需要资源」,送达后把 GPU 句柄写进
 * `*GpuCacheComponent`。组件知识只在这里 —— 服务与编排永远不认识
 * registry(J6-六修知识三层)。
 *
 *   作者组件(存盘)          本胶水                    运行期产物
 *   MeshComponent{mesh,mat} ─观察→ request/await ─送达→ MeshGpuCacheComponent
 *
 * ── 为什么这里可以用观察者(与 CLAUDE.md「异步就绪保留轮询」的关系)──
 *
 * 旧禁令的成因是「组件先构造、资产后到位,on_construct 只响一次,错过
 * 转换」。三件套下这个转换不再靠信号:on_construct 只登记**一次意图**
 * (await 进服务的等待名单 + request 发起管道),「资产后来到了」由管道
 * 终点经 `notifyReady/notifyFailed` **保证送达**(三条完成路都有归属,
 * J9-4)。等待名单替代了轮询;每帧遍历随 GpuResourceCache 一起退役(T12)。
 *
 * 代价是纪律收紧:可被观察的写入**只走 patch/replace**(J9-2)。直接
 * `get<C>(e).field = x` 的改动这里永远看不见。
 *
 * ── 相位与安全 ────────────────────────────────────────────────────────
 *
 *  - 观察者(进场/变更/离场)只做本地记账(标脏/挪离场队列),不改世界、
 *    不发命令(CLAUDE.md 观察者第一条 + 构建器未开)。
 *  - `tick`(帧 OPEN,构建器开)排空:离场摘组件,脏实体经
 *    `inComponentView` 复查后钉票 + await + request。
 *  - 送达回调在主线程帧 OPEN 相位执行(服务契约):验活 + 验仍引用后
 *    直接写 cache 组件 —— 不在任何 EnTT 信号里,安全。
 *  - RAII:等待票随记录亡(场景拆解自动退订);资产票 = 兴趣声明,
 *    **请求时**钉(边沿模型下没有每帧重钉,不先钉行会被归零收割)。
 *
 * ── 保留的三条组件语义(与 ResourceSubsystem 逐条对齐)────────────────
 *
 *  1. cache 组件**在 = 就绪**:没好时整个组件不存在(网格+材质合取)。
 *  2. 材质空句柄 = 作者没指定(合法);**不是**「还没好」。
 *  3. 换资产/在途/失败**保持旧组件**(旧图画到新图到位,不闪);旧资产
 *     票押到新副本落地才放(防「换资产窗口期画死句柄」)。
 *     材质终败换装 M_Missing 兜底(实体变色不消失,裁决七),原票保留
 *     —— 行活着,内容修复/重注册的失效推送才到得了这里,换装可逆。
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <lux/engine/resource/asset/AssetManager.hpp>       // acquire(兴趣票)/fetchAsset(材质分域)
#include <lux/engine/resource/asset/AssetRef.hpp>
#include <lux/engine/resource/asset/BuiltinAssetIds.hpp>    // M_Missing 兜底(裁决七)

#include <lux/engine/ecs/render/ResidencyCallbacks.hpp>
#include <lux/engine/ecs/render/IRenderSubsystem.hpp>
#include "lux/engine/ecs/render/RenderResourceEvents.hpp"   // 域/失败词汇 + 句柄位解包
#include "lux/engine/ecs/render/RenderViewUtil.hpp"         // Change/Leave 观察者 + inComponentView
#include "lux/engine/ecs/render/components/MeshGpuCacheComponent.hpp"
#include "lux/engine/ecs/render/components/TextureGpuCacheComponent.hpp"
#include "lux/engine/ecs/render/components/AssetStreamingStateComponent.hpp"

namespace lux::ecs
{
    /// 材质槽的域分派:MATERIAL 与 MATERIAL_INSTANCE 是两个上传域,按资产
    /// 壳的类型判(壳注册期即带类型,不要求数据已加载);未注册按 MATERIAL
    /// 走 —— 行会以终败收尾,失败观察可见。
    [[nodiscard]] inline EResourceDomain
    materialDomainOf(const lux::asset::AssetManager& mgr,
                     const lux::asset::asset_id_t&   id)
    {
        const auto* a = mgr.fetchAsset(id);
        return (a != nullptr
                && a->type() == lux::asset::EAssetType::MATERIAL_INSTANCE)
                   ? EResourceDomain::MATERIAL_INSTANCE
                   : EResourceDomain::MATERIAL;
    }

    /// 一类组件一个解析器(ResourceSubsystem 同款类型擦除:只为装进一个
    /// vector,每个解析器自身编译期特化)。
    class IResidencyResolver
    {
    public:
        virtual ~IResidencyResolver() = default;
        virtual void attach(lux::meta::EntityRegistry&) = 0;
        virtual void detach() = 0;
        /// 帧 OPEN 排空:离场摘组件 + 脏实体发起请求。
        virtual void drain(lux::meta::EntityRegistry&, lux::asset::AssetManager&) = 0;
        /// 失效命中集:摘死句柄组件、清送达态、重新标脏(兴趣票保留)。
        virtual void onInvalidated(
            const std::unordered_set<lux::asset::asset_id_t>&,
            lux::meta::EntityRegistry*) = 0;
        virtual void releaseAll() = 0;
    };

    /// 把 `C` 的某个 `asset_id_t` 字段驻留成 `TextureGpuCacheComponent`。
    /// 声明式注册同 ResourceSubsystem:
    ///     `resolveTextureOf<Image2DComponent, &Image2DComponent::texture>()`
    template <class C, lux::asset::asset_id_t C::*Field,
              class Exclude = ComponentList<>>
    class TextureResidencyResolver final : public IResidencyResolver
    {
        struct Rec
        {
            lux::asset::asset_id_t     want{};
            lux::asset::AssetRef       ref{};       ///< 兴趣票(请求时钉)
            ResidencyCallbacks::Ticket wait{};      ///< 在途等待(RAII)
            bool                       active{false};   ///< 本 want 已发起(幂等闸)
            bool                       failed{false};
            /// 换资产押下的旧票:新副本写进组件才放(防闪 + 防死句柄窗口)。
            std::vector<lux::asset::AssetRef> retired;
        };

    public:
        TextureResidencyResolver(const ResidencyCallbacks& cb,
                                 lux::asset::AssetManager& mgr) noexcept
            : cb_(&cb), mgr_(&mgr)
        {}

        void attach(lux::meta::EntityRegistry& reg) override
        {
            reg_ = &reg;
            leave_.attach(reg, [this](lux::meta::entity_id e) { onLeave(e); });
            change_.attach(reg, [this](lux::meta::entity_id e) { dirty_.insert(e); });
        }

        void detach() override
        {
            leave_.detach();
            change_.detach();
            reg_ = nullptr;
        }

        void drain(lux::meta::EntityRegistry& reg,
                   lux::asset::AssetManager&  mgr) override
        {
            for (const auto e : leaving_)
                if (reg.valid(e) && reg.template all_of<TextureGpuCacheComponent>(e))
                    reg.template remove<TextureGpuCacheComponent>(e);
            leaving_.clear();
            // 离场记录在此析构(票据/等待随之亡);先于脏处理 —— 同帧
            // 摘了又挂回来的实体走全新记录。
            leaving_recs_.clear();

            for (const auto e : dirty_)
            {
                if (!inComponentView<C>(reg, e, ComponentList<>{}, Exclude{}))
                    continue;   // 已离场/合取未满 —— 离场路径已经/将会收账
                const lux::asset::asset_id_t want = reg.template get<C>(e).*Field;

                if (want.is_nil())
                {   // 作者清空 —— 记录亡(票还)、组件撤(消费者当无贴图画)。
                    recs_.erase(e);
                    if (reg.template all_of<TextureGpuCacheComponent>(e))
                        reg.template remove<TextureGpuCacheComponent>(e);
                    continue;
                }

                Rec& r = recs_[e];
                if (r.active && r.want == want) continue;   // 去重 = 记录状态

                auto retired = std::move(r.retired);
                retired.push_back(std::move(r.ref));   // 旧票押住(空票无害)
                r.want    = want;
                r.failed  = false;
                r.active  = true;
                r.retired = std::move(retired);
                r.ref     = mgr.acquire(want);
                // 先 await 后 request:命中路在 request 内同步送达。
                r.wait = cb_->await(want,
                    [this, e, want](std::uint64_t bits, const ResourceFailure* f)
                    { onDeliver(e, want, bits, f); });
                cb_->request(want, EResourceDomain::TEXTURE);
            }
            dirty_.clear();
        }

        void onInvalidated(const std::unordered_set<lux::asset::asset_id_t>& ids,
                           lux::meta::EntityRegistry* reg) override
        {
            for (auto& [e, r] : recs_)
            {
                if (!ids.contains(r.want)) continue;
                // 句柄已死(级联销毁已执行):当帧摘组件,清送达态重新标脏。
                // 兴趣票**保留** —— 行经重建走干净路,计数不经 1→0。
                if (reg != nullptr && reg->valid(e)
                    && reg->template all_of<TextureGpuCacheComponent>(e))
                    reg->template remove<TextureGpuCacheComponent>(e);
                r.active = false;
                r.failed = false;
                r.wait.reset();
                r.retired.clear();   // 组件已摘,旧票没有防闪意义了
                dirty_.insert(e);
            }
        }

        void releaseAll() override
        {
            recs_.clear();
            leaving_.clear();
            leaving_recs_.clear();
            dirty_.clear();
        }

    private:
        void onLeave(lux::meta::entity_id e)
        {
            if (auto it = recs_.find(e); it != recs_.end())
            {
                // 只记账:票据归还与组件摘除都推迟到 drain(信号期不改世界)。
                leaving_.push_back(e);
                leaving_recs_.push_back(std::move(it->second));
                recs_.erase(it);
            }
            dirty_.erase(e);
        }

        void onDeliver(lux::meta::entity_id e, const lux::asset::asset_id_t& id,
                       std::uint64_t bits, const ResourceFailure* fail)
        {
            auto it = recs_.find(e);
            if (it == recs_.end() || it->second.want != id) return;   // 验仍引用
            Rec& r = it->second;
            r.wait.reset();
            if (fail != nullptr || bits == 0)
            {   // 终败:保持旧组件(不闪);失败观察经编排 failure_sink 可见。
                r.failed = true;
                return;
            }
            if (reg_ == nullptr || !reg_->valid(e)) return;           // 验活
            const auto h = unpackHandleBits<lux::render::RTextureHandle>(bits);
            if (auto* cc = reg_->template try_get<TextureGpuCacheComponent>(e))
                *cc = TextureGpuCacheComponent{h, id};
            else
                reg_->template emplace<TextureGpuCacheComponent>(
                    e, TextureGpuCacheComponent{h, id});
            r.retired.clear();   // 新副本落地,旧票此刻才放
        }

        const ResidencyCallbacks*  cb_{nullptr};
        lux::asset::AssetManager*  mgr_{nullptr};
        lux::meta::EntityRegistry* reg_{nullptr};

        std::unordered_map<lux::meta::entity_id, Rec> recs_;
        std::unordered_set<lux::meta::entity_id>      dirty_;
        std::vector<lux::meta::entity_id>             leaving_;
        std::vector<Rec>                              leaving_recs_;
        ComponentSetChangeObserver<C, ComponentList<>, Exclude> change_;
        ComponentSetLeaveObserver <C, ComponentList<>, Exclude> leave_;
    };

    /// 把 `C` 的网格 + 材质两个字段驻留成 `MeshGpuCacheComponent`。
    /// 「材质 nil = 作者没设」与「材质没好 = 整个组件等着」分开判
    /// (组件语义三条,见文件头);材质终败换装 M_Missing。
    template <class C, lux::asset::asset_id_t C::*MeshField,
                       lux::asset::asset_id_t C::*MaterialField,
              class Exclude = ComponentList<>>
    class MeshResidencyResolver final : public IResidencyResolver
    {
        struct Rec
        {
            lux::asset::asset_id_t     want_mesh{};
            lux::asset::asset_id_t     want_mat{};
            lux::asset::AssetRef       mesh_ref{};
            lux::asset::AssetRef       mat_ref{};   ///< 原材质票,终败也不放(见文件头)
            ResidencyCallbacks::Ticket mesh_wait{};
            ResidencyCallbacks::Ticket mat_wait{};
            std::uint64_t              mesh_bits{0};
            std::uint64_t              mat_bits{0};
            bool                       mesh_failed{false};
            bool                       active{false};
            std::uint32_t              generation{0};
            // 材质终败的 M_Missing 换装态(原票之外另钉兜底)。
            bool                       fallback{false};
            lux::asset::AssetRef       fb_ref{};
            ResidencyCallbacks::Ticket fb_wait{};
            std::uint64_t              fb_bits{0};
            std::vector<lux::asset::AssetRef> retired;   ///< 同 TextureResidencyResolver
        };

    public:
        MeshResidencyResolver(const ResidencyCallbacks& cb,
                              lux::asset::AssetManager& mgr) noexcept
            : cb_(&cb), mgr_(&mgr)
        {}

        void attach(lux::meta::EntityRegistry& reg) override
        {
            reg_ = &reg;
            leave_.attach(reg, [this](lux::meta::entity_id e) { onLeave(e); });
            change_.attach(reg, [this](lux::meta::entity_id e) { dirty_.insert(e); });
        }

        void detach() override
        {
            leave_.detach();
            change_.detach();
            reg_ = nullptr;
        }

        void drain(lux::meta::EntityRegistry& reg,
                   lux::asset::AssetManager&  mgr) override
        {
            for (const auto e : leaving_)
            {
                if (reg.valid(e) && reg.template all_of<MeshGpuCacheComponent>(e))
                    reg.template remove<MeshGpuCacheComponent>(e);
                if (reg.valid(e) &&
                    reg.template all_of<AssetStreamingStateComponent>(e))
                {
                    reg.template remove<AssetStreamingStateComponent>(e);
                }
            }
            leaving_.clear();
            leaving_recs_.clear();

            for (const auto e : dirty_)
            {
                if (!inComponentView<C>(reg, e, ComponentList<>{}, Exclude{}))
                    continue;
                const C& c = reg.template get<C>(e);
                const lux::asset::asset_id_t wm   = c.*MeshField;
                const lux::asset::asset_id_t wmat = c.*MaterialField;

                if (wm.is_nil())
                {   // 没网格就没什么可画的。
                    recs_.erase(e);
                    if (reg.template all_of<MeshGpuCacheComponent>(e))
                        reg.template remove<MeshGpuCacheComponent>(e);
                    if (reg.template all_of<AssetStreamingStateComponent>(e))
                        reg.template remove<AssetStreamingStateComponent>(e);
                    continue;
                }

                Rec& r = recs_[e];
                if (r.active && r.want_mesh == wm && r.want_mat == wmat)
                    continue;

                auto retired = std::move(r.retired);
                retired.push_back(std::move(r.mesh_ref));
                retired.push_back(std::move(r.mat_ref));
                retired.push_back(std::move(r.fb_ref));
                r.want_mesh   = wm;
                r.want_mat    = wmat;
                r.mesh_bits   = 0;
                r.mat_bits    = 0;
                r.fb_bits     = 0;
                r.mesh_failed = false;
                r.fallback    = false;
                r.active      = true;
                if (++r.generation == 0u)
                    ++r.generation;
                r.mesh_wait.reset();
                r.mat_wait.reset();
                r.fb_wait.reset();
                r.retired  = std::move(retired);
                r.mesh_ref = mgr.acquire(wm);
                if (!wmat.is_nil()) r.mat_ref = mgr.acquire(wmat);

                const AssetStreamingStateComponent streaming{
                    r.generation,
                    EAssetStreamingPhase::LOADING,
                    kDefaultStreamingFeedbackStyle};
                if (reg.template all_of<AssetStreamingStateComponent>(e))
                    reg.template replace<AssetStreamingStateComponent>(
                        e,
                        streaming);
                else
                    reg.template emplace<AssetStreamingStateComponent>(
                        e,
                        streaming);

                // 全部 await 就位再 request(命中路同步送达时合取才判得全)。
                r.mesh_wait = cb_->await(wm,
                    [this, e, wm](std::uint64_t bits, const ResourceFailure* f)
                    { onMeshDeliver(e, wm, bits, f); });
                if (!wmat.is_nil())
                    r.mat_wait = cb_->await(wmat,
                        [this, e, wmat](std::uint64_t bits, const ResourceFailure* f)
                        { onMatDeliver(e, wmat, bits, f); });
                cb_->request(wm, EResourceDomain::MESH);
                if (!wmat.is_nil())
                    cb_->request(wmat, materialDomainOf(mgr, wmat));
            }
            dirty_.clear();
        }

        void onInvalidated(const std::unordered_set<lux::asset::asset_id_t>& ids,
                           lux::meta::EntityRegistry* reg) override
        {
            const auto fid = lux::asset::builtinMissingMaterialId();
            for (auto& [e, r] : recs_)
            {
                const bool hit = ids.contains(r.want_mesh)
                    || (!r.want_mat.is_nil() && ids.contains(r.want_mat))
                    || (r.fallback && ids.contains(fid));
                if (!hit) continue;
                if (reg != nullptr && reg->valid(e)
                    && reg->template all_of<MeshGpuCacheComponent>(e))
                    reg->template remove<MeshGpuCacheComponent>(e);
                r.active      = false;
                r.mesh_bits   = 0;
                r.mat_bits    = 0;
                r.fb_bits     = 0;
                r.mesh_failed = false;
                r.fallback    = false;
                r.mesh_wait.reset();
                r.mat_wait.reset();
                r.fb_wait.reset();
                r.fb_ref = {};
                r.retired.clear();
                if (++r.generation == 0u)
                    ++r.generation;
                if (reg != nullptr && reg->valid(e))
                {
                    const AssetStreamingStateComponent streaming{
                        r.generation,
                        EAssetStreamingPhase::LOADING,
                        kDefaultStreamingFeedbackStyle};
                    if (reg->template all_of<AssetStreamingStateComponent>(e))
                        reg->template replace<AssetStreamingStateComponent>(
                            e,
                            streaming);
                    else
                        reg->template emplace<AssetStreamingStateComponent>(
                            e,
                            streaming);
                }
                dirty_.insert(e);
            }
        }

        void releaseAll() override
        {
            recs_.clear();
            leaving_.clear();
            leaving_recs_.clear();
            dirty_.clear();
        }

    private:
        void onLeave(lux::meta::entity_id e)
        {
            if (auto it = recs_.find(e); it != recs_.end())
            {
                leaving_.push_back(e);
                leaving_recs_.push_back(std::move(it->second));
                recs_.erase(it);
            }
            dirty_.erase(e);
        }

        void onMeshDeliver(lux::meta::entity_id e, const lux::asset::asset_id_t& id,
                           std::uint64_t bits, const ResourceFailure* fail)
        {
            auto it = recs_.find(e);
            if (it == recs_.end() || it->second.want_mesh != id) return;
            Rec& r = it->second;
            r.mesh_wait.reset();
            if (fail != nullptr || bits == 0)
            {   // 网格终败无兜底:保持旧组件;失败观察经 failure_sink 可见。
                r.mesh_failed = true;
                if (reg_ != nullptr && reg_->valid(e) &&
                    reg_->template all_of<AssetStreamingStateComponent>(e))
                {
                    reg_->template remove<AssetStreamingStateComponent>(e);
                }
                return;
            }
            r.mesh_bits = bits;
            tryWrite(e, r);
        }

        void onMatDeliver(lux::meta::entity_id e, const lux::asset::asset_id_t& id,
                          std::uint64_t bits, const ResourceFailure* fail)
        {
            auto it = recs_.find(e);
            if (it == recs_.end() || it->second.want_mat != id) return;
            Rec& r = it->second;
            r.mat_wait.reset();
            if (fail != nullptr || bits == 0)
            {   // 「加载中」与「确认坏/没了」分开判(裁决七):终败才换装
                // M_Missing —— 实体变色,不消失。原票保留(可逆,见文件头)。
                startFallback(e, r);
                return;
            }
            r.mat_bits = bits;
            tryWrite(e, r);
        }

        void onFallbackDeliver(lux::meta::entity_id e, std::uint64_t bits,
                               const ResourceFailure* fail)
        {
            auto it = recs_.find(e);
            if (it == recs_.end() || !it->second.fallback) return;
            Rec& r = it->second;
            r.fb_wait.reset();
            if (fail != nullptr || bits == 0) return;   // 兜底自己也败:保持旧样
            r.fb_bits = bits;
            tryWrite(e, r);
        }

        void startFallback(lux::meta::entity_id e, Rec& r)
        {
            const auto fid = lux::asset::builtinMissingMaterialId();
            r.fallback = true;
            r.fb_ref   = mgr_->acquire(fid);
            r.fb_wait  = cb_->await(fid,
                [this, e](std::uint64_t bits, const ResourceFailure* f)
                { onFallbackDeliver(e, bits, f); });
            cb_->request(fid, EResourceDomain::MATERIAL);
        }

        /// 合取写入:网格就绪 && (材质 nil | 就绪 | 兜底就绪) 才写组件。
        void tryWrite(lux::meta::entity_id e, Rec& r)
        {
            if (reg_ == nullptr || !reg_->valid(e)) return;   // 验活
            if (r.mesh_bits == 0) return;
            lux::render::RMaterialHandle mat{};
            lux::asset::asset_id_t       mat_src{};
            if (!r.want_mat.is_nil())
            {
                if (r.mat_bits != 0)
                {
                    mat     = unpackHandleBits<lux::render::RMaterialHandle>(r.mat_bits);
                    mat_src = r.want_mat;
                }
                else if (r.fallback && r.fb_bits != 0)
                {
                    mat     = unpackHandleBits<lux::render::RMaterialHandle>(r.fb_bits);
                    mat_src = lux::asset::builtinMissingMaterialId();
                }
                else
                    return;   // 作者指定了材质但还没好 —— 等它
            }
            const auto mesh = unpackHandleBits<lux::render::RMeshHandle>(r.mesh_bits);
            // ★ 批 R0(反应式抽取):此前是 `*cc = MeshGpuCacheComponent{…}` 直写。
            //   直写**不发 `on_update` 信号**(CLAUDE.md 已入册,
            //   `reactive_storage_probe` ③a 给了实证锚点),于是「句柄换代了」这件事
            //   下游只能靠每帧全扫重新发现 —— 那正是 MeshInstance 那 74.8 µs 的一部分。
            //   改成 `replace<>` 之后这条转换是**可被观察的**。
            //   R0 阶段还没有消费者连上,行为不变;信号派发到空 sigh 近乎免费。
            if (reg_->template all_of<MeshGpuCacheComponent>(e))
                reg_->template replace<MeshGpuCacheComponent>(
                    e, MeshGpuCacheComponent{mesh, mat, r.want_mesh, mat_src});
            else
                reg_->template emplace<MeshGpuCacheComponent>(
                    e, MeshGpuCacheComponent{mesh, mat, r.want_mesh, mat_src});
            if (reg_->template all_of<AssetStreamingStateComponent>(e))
                reg_->template remove<AssetStreamingStateComponent>(e);
            r.retired.clear();
        }

        const ResidencyCallbacks*  cb_{nullptr};
        lux::asset::AssetManager*  mgr_{nullptr};
        lux::meta::EntityRegistry* reg_{nullptr};

        std::unordered_map<lux::meta::entity_id, Rec> recs_;
        std::unordered_set<lux::meta::entity_id>      dirty_;
        std::vector<lux::meta::entity_id>             leaving_;
        std::vector<Rec>                              leaving_recs_;
        ComponentSetChangeObserver<C, ComponentList<>, Exclude> change_;
        ComponentSetLeaveObserver <C, ComponentList<>, Exclude> leave_;
    };

    /// 驻留胶水 —— 解析器集合 + 失效观察接线。
    ///
    /// ★ 批 B1 起它是一个**普通的 schedule node**(`ISystem`),不再是
    ///   `RenderSystem` 里被特殊对待的那一个 `IRenderSubsystem`。
    ///
    ///   这一步几乎是免费的:它此前的 `tick`/`releaseRefs` 都**收下
    ///   `SceneRenderBinding` 然后忽略** —— 它要的只有 AssetManager、回调和
    ///   registry。所谓「渲染子系统」的身份自始至终没被它用到。
    ///
    ///   「排在消费者之前」从此是一条**类型化的边**:消费方声明
    ///   `runsAfter<ResidencySubsystem>()`(由消费者声明依赖,不由本类声明它的
    ///   消费者是谁),而不是「被插到 vector 的第 0 位」。
    class ResidencySubsystem final : public IRenderSubsystem
    {
    public:
        explicit ResidencySubsystem(lux::asset::AssetManager& mgr) noexcept
            : mgr_(&mgr)
        {}

        ~ResidencySubsystem() override { detach(); }

        /// 装配期接线(宿主/引擎;attach 之前)。未接线 = 惰性:不请求、
        /// 不送达 —— 组件语义退化为「永远没好」,不崩不漏。
        void setCallbacks(ResidencyCallbacks cb) { cb_ = std::move(cb); }

        /// 声明「这个组件的这个字段是一张贴图」。装配期调用,第一次 tick
        /// 之前(ResourceSubsystem 同款声明面)。
        template <class C, lux::asset::asset_id_t C::*Field,
                  class Exclude = ComponentList<>>
        void resolveTextureOf()
        {
            resolvers_.push_back(
                std::make_unique<TextureResidencyResolver<C, Field, Exclude>>(
                    cb_, *mgr_));
        }

        /// 声明「这个组件的这两个字段是网格与材质」。
        template <class C, lux::asset::asset_id_t C::*MeshField,
                           lux::asset::asset_id_t C::*MaterialField,
                  class Exclude = ComponentList<>>
        void resolveMeshOf()
        {
            resolvers_.push_back(
                std::make_unique<MeshResidencyResolver<C, MeshField,
                                                       MaterialField, Exclude>>(
                    cb_, *mgr_));
        }

        // Residency is data preparation, not the render consumer. Mesh and
        // material feature requirements are declared by MeshSubsystem /
        // SkeletalMeshSubsystem, which are the nodes that actually submit the
        // operations. Keeping resolver configuration out of renderFeatures()
        // also makes an ISystem descriptor immutable across transactional
        // resolver installation.

        /// 连信号。**在 `onAdded` 里做** —— 而 `onAdded` 由
        /// `ScheduleBuilder::commit` 触发,那时包的 `resolveTextureOf` 已经全部
        /// 调过了。此前它由 `RenderSystem` 在第一次 update 时统一调,靠的是
        /// 「注册子系统一定发生在首帧之前」这条约定;现在是装配事务保证的。
        void onAdded(const SystemSetupContext& setup) override
        {
            attach(setup.registry());
        }

        void onRemoved(const SystemRemovalContext&) override { detach(); }

        void update(RenderSubsystemContext& ctx) override
        {
            drainResolvers(ctx.registry());
        }

        void close(RenderSubsystemContext&) noexcept override
        {
            releaseAll();
        }

        /// 还掉全部驻留兴趣票。宿主在场景拆解时显式调 —— 票据释放是 CPU 侧回调
        /// (`Ticket::reset` 调一个 `move_only_function`),**不需要帧开着**;
        /// 由此产生的 GPU 回收归驻留服务自己的通道。
        void releaseAll()
        {
            for (auto& r : resolvers_) r->releaseAll();
        }

        /// 排空脏实体。前置:帧 OPEN、构建器开(`update` 由 `Schedule::tick` 驱动,
        /// 宿主的帧序保证这一点)。手工驱动的探针/可视化 demo 直接调它。
        void drainResolvers(lux::meta::EntityRegistry& reg)
        {
            if (!cb_.request || !cb_.await) return;   // 未接线惰性
            for (auto& r : resolvers_) r->drain(reg, *mgr_);
        }

        /// 连/断信号。`Schedule` 经 `onAdded`/`onRemoved` 自动做;**手工驱动**的
        /// 探针与可视化 demo(不走 `Schedule::tick`)自己调 —— 与 `drainResolvers`
        /// 对称:手工驱动者手动做调度器自动做的那两件事。
        void attach(lux::meta::EntityRegistry& reg)
        {
            reg_ = &reg;
            for (auto& r : resolvers_) r->attach(reg);
            if (cb_.watch_invalidation)
                watch_ = cb_.watch_invalidation(
                    [this](const std::vector<lux::asset::asset_id_t>& ids)
                    {
                        std::unordered_set<lux::asset::asset_id_t> set(
                            ids.begin(), ids.end());
                        for (auto& r : resolvers_) r->onInvalidated(set, reg_);
                    });
        }

        void detach() noexcept
        {
            watch_.reset();
            for (auto& r : resolvers_) r->detach();
            reg_ = nullptr;
        }

    private:
        lux::asset::AssetManager*  mgr_{nullptr};
        lux::meta::EntityRegistry* reg_{nullptr};
        ResidencyCallbacks         cb_{};
        ResidencyCallbacks::Ticket watch_{};
        std::vector<std::unique_ptr<IResidencyResolver>> resolvers_;
    };

} // namespace lux::ecs
