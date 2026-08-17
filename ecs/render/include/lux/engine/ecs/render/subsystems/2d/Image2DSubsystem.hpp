#pragma once
// ============================================================================
//  Image2DSubsystem.hpp — ECS Image2DComponent → GPU-resident Canvas2D instance
//  (lux::ecs, v2 — .internal/2d-gpu-driven-rewrite.md §3.5).
//
//  RETAINED subsystem: each image entity owns ONE GPU-resident instance in the
//  scene's Canvas2D arena, created on first sight (AddImage2D, reply-validated
//  per G-05), updated by DELTAS and removed when the entity leaves. The wire
//  carries change, never content:
//    - transform: the world affine is re-baked each frame (world × size ×
//      pivot — 2D, ~12 FLOPs) and VALUE-compared against the last-sent bake;
//      only a difference emits a batch entry. Value-based on purpose (the
//      no-trust axiom): a direct field write to Transform2D / size / pivot
//      with no dirty flag still registers.
//    - visual (uv/tint/texture) and key (priority/visible): value-compared
//      against the last-sent copy; changes emit their low-frequency op.
//  A fully static image set therefore sends NOTHING — no heartbeat, no
//  producer staging; those v1 concepts are gone.
//
//  ── 它**不再认识资产**（新工作线阶段 2）────────────────────────────────────
//
//  此前这里有 `ensureTexture` + `acquireTexture` / `releaseTexture` + 一个每帧的
//  贴图 memo：一个画图的子系统，自己管着异步上传状态机和一本引用计数账。现在那些
//  全归驻留胶水(`ResidencySubsystem`),这里只 `try_get<TextureGpuCacheComponent>`:
//
//    · 有那个组件 → 拿 `handle.index` 当 bindless 索引；
//    · 没有       → `kNoTexture`，照旧画成纯 tint —— 「还没就绪」与「作者本来就
//                   没设贴图」在这里是**同一个**状态，不必各写一条路径。
//
//  于是本文件里 `lux::asset` 一次都不出现。
//
//  No per-frame camera gate here: instances persist server-side, and DRAWING
//  is gated by the retained scene bit (SetCanvas2DEnabled), which the
//  Camera2DUploadSubsystem flips edge-triggered on the publishable-camera state.
//
//  Teardown is scene-domain (per the v2 architecture): instances die with destroyScene;
//  releaseRefs drops scoped in-flight continuations and clears local bookkeeping.
// ============================================================================

#include <lux/engine/ecs/render/IRenderSubsystem.hpp>
#include <lux/engine/ecs/render/RenderSpatialTransform.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>       // ctx.canvas2d() / scene()
#include <lux/engine/ecs/render/components/TextureGpuCacheComponent.hpp>     // 解析好的贴图句柄(资源子系统产出)
#include <lux/engine/ecs/render/RenderViewUtil.hpp>               // ComponentSetLeaveObserver
#include <lux/engine/ecs/render/TrackedRenderRequest.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/SpatialTransformMath.hpp>
#include <lux/engine/ecs/render/components/2d/Order2DComponents.hpp>       // A2-03: Y-sort / parallax
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/function/render/client/features/canvas2d/Canvas2DOperation.hpp>  // Image2DInstanceData / Image2DHandle
#include <lux/engine/function/render/client/RenderRequest.hpp>                      // in-flight create handle
#include <lux/engine/meta/LuxObject.hpp>   // EntityRegistry / entity_id

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class Camera2DUploadSubsystem;
    class ResidencySubsystem;

    /// ★ 批 B4 起它是一个**普通的 schedule node**(`ISystem`)。渲染绑定由构造
    ///   注入,不再每帧由 `RenderSystem` 的调度循环递进来。
    ///
    ///   它的 `RenderRequest::then` **不迁 sender**(批 C2)—— 裁决与三条理由
    ///   记在 `PooledSlotSubsystem.hpp` 的类头。
    class Image2DSubsystem final : public lux::ecs::IRenderSubsystem
    {
    public:
        Image2DSubsystem() = default;

        ~Image2DSubsystem() override { leave_.detach(); detachStateSignal(); }

        /// Canvas2D 的实例竞技场就是它画的地方。
        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept override
        {
            static const std::string_view kFeatures[] = { "Canvas2D" };
            return kFeatures;
        }

        [[nodiscard]] std::span<const RenderSubsystemType>
        runsAfter() const noexcept override
        {
            static constexpr RenderSubsystemType kAfter[] = {
                renderSubsystemType<ResidencySubsystem>(),
                renderSubsystemType<Camera2DUploadSubsystem>()
            };
            return kAfter;
        }

        /// One live GPU instance + the LAST-SENT state every diff compares against.
        ///
        /// ★ 批 R4:它是一个**组件**,不再是子系统私有的 `unordered_map` 侧表 ——
        ///   理由与 `MeshInstanceStateComponent.hpp` 的文件头完全一样(逐实体数据
        ///   的归属就是组件池:省掉每帧每实体一次哈希+指针跳转、能被 view 迭代、
        ///   随实体自动消亡、可检视)。
        ///
        ///   ⚠️ 它**嵌在子系统里**而不是单开一个头:只有本系统读写、绝不序列化、
        ///   不进属性面板、没有第二个消费者。嵌套正好表达这层归属。
        ///   (MeshInstance 那份是模板,才需要独立的头。)
        ///
        ///   「这个实体有没有 GPU 实例」由本组件是否存在回答,于是归还路径收敛成
        ///   一条 `on_destroy<Live>` —— 实体销毁与离开集合两条路一个处理器,
        ///   句柄在信号里仍然可读。
        struct Live
        {
            lux::render::Image2DHandle  handle{};
            float                       m[6]{};      ///< last sent baked affine
            std::int32_t                page_delta[2]{};
            float                       uv[4]{};
            std::uint32_t               tint{0};
            std::uint32_t               tex{lux::render::kNoTexture};
            float                       priority{0.f};
            bool                        visible{true};
            std::uint8_t                group{0};    ///< A2-04 offscreen group
            // ★ 这里此前还有一个 `tex_id`：本实例在共享贴图缓存里钉住的那个 asset id。
            //   引用计数归资源子系统之后就不需要了 —— 谁用了哪张图，由那边按实体记。
        };

        /// G-05 failure record: a failed create must not re-issue every frame.
        /// InvalidConfiguration / generic Unknown are permanent (the scene has no
        /// Canvas2D — futile until the scene is rebuilt); CapacityExhausted is
        /// transient and retried after a bounded backoff.
        struct FailRecord
        {
            bool permanent{false};
            bool dispatch_reported{false};
            bool reply_reported{false};
            int  retry_in{0};
        };
        static constexpr int kTransientRetryDrives = 120;   // ~2s @ 60fps

    public:
        void update(RenderSubsystemContext& uctx) override
        {
            auto& registry = uctx.registry();
            auto& ctx = uctx.render();

            // Capacity failures are rare, but they need an explicit wake-up:
            // no component signal is produced merely because backoff time passed.
            for (const auto& [e, failure] : failed_)
                if (!failure.permanent)
                    changes_.mark(e);

            // Parallax origin: the active camera's world centre (A2-03). One
            // lookup per tick; scenes without a camera get a zero origin.
            lux::spatial::Position2D camera_position{};
            for (auto ce : registry.view<PrimaryCameraTag, ResolvedTransform2DComponent>())
            {
                const auto& cw = registry.get<ResolvedTransform2DComponent>(ce);
                camera_position = cw.position;
                break;
            }
            // ★ 批 R4:相机动了 → **所有视差图**的烘焙平移都变了,而它们自己的
            //   组件一个都没变,信号驱动够不着 —— 必须由知道这份外部输入的人推一把。
            //   这是本节点相对 MeshInstance **多出来的一个形状**,也是把机制套到
            //   第二个节点时才暴露出来的(`markAllWith` 就是为它加的)。
            const bool cam_moved = !camera_position_seeded_ ||
                camera_position != last_camera_position_;
            if (cam_moved)
            {
                last_camera_position_ = camera_position;
                camera_position_seeded_ = true;
                changes_.markAllWith<Parallax2DComponent>();
            }

            // 稳态快速出口:下面那句 `ctx.canvas2d()` 是一次**按字符串**查进程目录。
            if (changes_.empty() && !create_requests_.hasCompletions() &&
                to_unbind_.empty() && leaving_.empty())
            {
#if !LUX_ECS_EXTRACTION_VERIFY
                return;
#endif
            }

            auto canvas = ctx.canvas2d();
            if (!canvas.valid()) return;   // scene without Canvas2D → zero cost (R2-04 contract)

            batch_.clear();

            // ① 消费值回执。同步完成与异步完成走同一路径；已经离场或被新
            //    意图取代的成功回执不再发布，而是进入下面的归还队列。
            drainCreateCompletions(registry);

            // ② 摘掉离场实体的状态组件 —— 触发 on_destroy<Live>,句柄进 leaving_。
            for (const auto e : to_unbind_)
                if (registry.valid(e) && registry.all_of<Live>(e))
                    registry.remove<Live>(e);
            to_unbind_.clear();

            // ③ 先还后借：排空离场队列。信号只把句柄读走记账 —— removeImage 是渲染
            // 命令，必须在构建器开着的时候发，而实体销毁那一刻通常不是。
            for (const auto h : leaving_)
                removeImage(canvas, ctx.scene(), h);
            leaving_.clear();

            // ④ 逐实体处理:换出语义与 VERIFY 对拍都在 drain 里。
            changes_.drain(registry, renderSubsystemType<Image2DSubsystem>().name,
                [&](lux::meta::entity_id e, Image2DComponent& sp)
                {
                    return processEntity(registry, ctx, canvas, camera_position, e, sp,
                                         registry.get<ResolvedTransform2DComponent>(e));
                });

            // ★ 此前这是第二个相位 `flush(ctx)`（`RenderSystem` 对全部子系统跑完
            //   tick 之后再统一跑一遍）。折进本节点 update 的末尾是安全的:`batch_`
            //   完全来自**本节点自己**这一轮的累积,不依赖任何别的节点的 tick。
            if (!batch_.empty())
                updateTransforms(ctx.canvas2d(), batch_);   // ONE bulk op for every dirty image
        }

    private:
        /// 一个实体的全部维护。@return 本次是否做了实质动作(VERIFY oracle 的判据)。
        bool processEntity(lux::meta::EntityRegistry& registry, SceneRenderBinding& ctx,
                           auto& canvas,
                           const lux::spatial::Position2D& camera_position,
                           lux::meta::entity_id e, const Image2DComponent& sp,
                           const ResolvedTransform2DComponent& wt)
        {
            bool did_work = false;
            {
                    // Bake the 2D affine: world × Scale(size), pivot shift folded into
                    // the translation so the normalised pivot sits at the entity origin
                    // (pivot 0.5,0.5 = centred → zero offset). Column-major world 4x4 →
                    // 6-float 2D affine (the render side expands a unit ±0.5 quad).
                    float m[6];
                    m[0] = wt.linear(0, 0) * sp.size.x();
                    m[1] = wt.linear(1, 0) * sp.size.x();
                    m[2] = wt.linear(0, 1) * sp.size.y();
                    m[3] = wt.linear(1, 1) * sp.size.y();
                    const float px = 0.5f - sp.pivot.x(), py = 0.5f - sp.pivot.y();
                    auto spatial = wt.position;
                    spatial.x += static_cast<double>(m[0] * px + m[2] * py);
                    spatial.y += static_cast<double>(m[1] * px + m[3] * py);
                    if (!lux::spatial::isFinite(spatial))
                        return false;

                    // A2-03 opt-in modifiers, applied at BAKE time (never written
                    // back to the authored components):
                    //  - parallax shifts the sent translation by cam×(1−factor);
                    //    the value diff below gives the right wire economy for free;
                    //  - y-sort derives the EFFECTIVE priority from world Y.
                    if (const auto* pl = registry.try_get<Parallax2DComponent>(e))
                    {
                        if (!offsetByScaledPosition(
                                spatial,
                                camera_position,
                                Eigen::Vector2f::Ones() - pl->factor))
                        {
                            return false;
                        }
                    }
                    const auto render_position = makeRenderLargePosition(
                        spatial, ctx.sceneOriginTile3D());
                    if (!render_position)
                    {
                        ctx.requestSceneOriginRebase(spatial);
                        return false;
                    }
                    m[4] = render_position->local[0];
                    m[5] = render_position->local[1];
                    const std::int32_t page_delta[2]{
                        render_position->page_delta[0],
                        render_position->page_delta[1]};
                    float prio = sp.priority;
                    if (const auto* ys = registry.try_get<YSort2DComponent>(e))
                        prio = ys->effectivePriority(spatial.y);

                    // 贴图：资源子系统解析好了就有这个组件。没有 = 还没就绪 **或者**
                    // 作者压根没设 —— 两者对本子系统是同一件事：这一帧画纯 tint。
                    // 组件之后出现时，下面的值比较自然把它升上去，不需要特殊路径。
                    const auto* tc = registry.try_get<TextureGpuCacheComponent>(e);
                    const std::uint32_t tex =
                        tc ? tc->handle.index : lux::render::kNoTexture;

                    if (Live* const state = registry.try_get<Live>(e))
                    {
                        Live& L = *state;

                        if (std::memcmp(m, L.m, sizeof(m)) != 0 ||
                            std::memcmp(
                                page_delta,
                                L.page_delta,
                                sizeof(page_delta)) != 0)
                        {
                            did_work = true;
                            lux::render::Image2DTransformEntry ent{};
                            ent.scene  = ctx.scene();
                            ent.handle = L.handle;
                            std::memcpy(ent.m, m, sizeof(m));
                            std::memcpy(
                                ent.page_delta,
                                page_delta,
                                sizeof(page_delta));
                            batch_.push_back(ent);
                            std::memcpy(L.m, m, sizeof(m));
                            std::memcpy(
                                L.page_delta,
                                page_delta,
                                sizeof(page_delta));
                        }
                        const float uv[4] = {sp.uv_rect.x(), sp.uv_rect.y(),
                                             sp.uv_rect.z(), sp.uv_rect.w()};
                        if (std::memcmp(uv, L.uv, sizeof(uv)) != 0 ||
                            sp.tint != L.tint || tex != L.tex)
                        {
                            updateVisual(canvas, ctx.scene(), L.handle, uv, sp.tint, tex);
                            std::memcpy(L.uv, uv, sizeof(uv));
                            L.tint = sp.tint;
                            L.tex  = tex;
                            did_work = true;
                        }
                        if (prio != L.priority || sp.visible != L.visible || sp.group != L.group)
                        {
                            updateKey(canvas, ctx.scene(), L.handle, prio, sp.visible, sp.group);
                            L.priority = prio;
                            L.visible  = sp.visible;
                            L.group    = sp.group;
                            did_work = true;
                        }
                        return did_work;
                    }
                    if (create_requests_.contains(e)) return did_work;

                    Live snap{};
                    std::memcpy(snap.m, m, sizeof(m));
                    std::memcpy(
                        snap.page_delta,
                        page_delta,
                        sizeof(page_delta));
                    snap.uv[0] = sp.uv_rect.x(); snap.uv[1] = sp.uv_rect.y();
                    snap.uv[2] = sp.uv_rect.z(); snap.uv[3] = sp.uv_rect.w();
                    snap.tint     = sp.tint;
                    snap.tex      = tex;
                    snap.priority = prio;
                    snap.visible  = sp.visible;
                    snap.group    = sp.group;

                    if (auto fit = failed_.find(e); fit != failed_.end())
                    {
                        if (fit->second.permanent) return did_work;
                        if (fit->second.retry_in > 0)
                        {
                            --fit->second.retry_in;
                            return did_work;
                        }
                        // Preserve diagnostic de-duplication while the retry
                        // generation is in flight.
                    }

                    // First sight: create with the FULL current state.
                    lux::render::Image2DInstanceData d{};
                    std::memcpy(d.m, m, sizeof(m));
                    std::memcpy(
                        d.page_delta,
                        page_delta,
                        sizeof(page_delta));
                    d.uv[0] = sp.uv_rect.x(); d.uv[1] = sp.uv_rect.y();
                    d.uv[2] = sp.uv_rect.z(); d.uv[3] = sp.uv_rect.w();
                    d.tint             = sp.tint;
                    d.texture_bindless = tex;

                    const auto started = create_requests_.start(
                        e,
                        snap,
                        [&]()
                        {
                            return addImage(
                                canvas,
                                ctx.scene(),
                                d,
                                prio,
                                sp.visible,
                                sp.group
                            );
                        }
                    );
                    if (started == ETrackedRequestStart::STARTED)
                        did_work = true;
                    else if (started == ETrackedRequestStart::INVALID_REQUEST)
                    {
                        auto& failure = rememberFailure(e);
                        failure.permanent = true;
                        failure.retry_in = 0;
                        if (!failure.reply_reported)
                        {
                            diagnoseRenderBridge(
                                "[Image2DSubsystem] add image produced an invalid "
                                "request; creation is latched until scene rebuild"
                            );
                            failure.reply_reported = true;
                        }
                    }
            }
            return did_work;
        }

    public:
        void onAdded(const SystemSetupContext& setup) override
        {
            auto& registry = setup.registry();
            leave_.attach(registry, [this](lux::meta::entity_id e) { onLeave(e); });
            attachStateSignal(registry);
            attachChangeSources(registry);
        }
        void onRemoved(const SystemRemovalContext&) override
        {
            leave_.detach();
            detachStateSignal();
            changes_.detach();
        }

        // 本节点不需要一个「帧开着」的显式拆解点(而 ResidencySubsystem
        // 与 CameraViewSubsystem 需要):判据是**要不要发渲染命令**。
        //     · 实例 scene-owned → destroyScene 整体回收,逐个 remove 是多余命令;
        //     · 贴图引用计数不在本节点 → 那本账在资源子系统,它自己收;
        //     · 在途续体由 `TrackedRenderRequest` 内部的词法请求所有者同步解绑。
        //   三条都不发命令，成员析构因此就是完整的本地收场。

    private:
        using Require = ComponentList<ResolvedTransform2DComponent>;
        using Exclude = ComponentList<>;

        TrackedRenderRequest<
            lux::meta::entity_id,
            lux::render::Image2DSlotReply,
            Live> create_requests_;
        std::unordered_map<lux::meta::entity_id, FailRecord> failed_;
        std::vector<lux::render::Image2DTransformEntry> batch_;   // dirty transforms this frame
        /// 已离开组件集合、状态组件还没摘掉的实体(实体本身仍然活着)。
        std::vector<lux::meta::entity_id> to_unbind_;
        /// 已离场、`removeImage` 还没发出去的实例句柄。信号填，`update` 开头排空。
        std::vector<lux::render::Image2DHandle> leaving_;

        /// ★ 批 R4:变更驱动的入口。稳态下它是空的。
        ExtractionChangeSet<Image2DComponent, Require, Exclude> changes_;
        /// 上一帧的视差原点(相机世界中心)。它变了 = 所有视差图的烘焙平移都变了,
        /// 而那**不产生任何针对这些实体的组件信号** —— 见 markAllWith 的说明。
        lux::spatial::Position2D last_camera_position_{};
        bool camera_position_seeded_{false};

        ComponentSetLeaveObserver<Image2DComponent, Require, Exclude> leave_;
        /// `on_destroy<Live>` 的连接靠它解绑。
        lux::meta::EntityRegistry* reg_{nullptr};

        [[nodiscard]] FailRecord& rememberFailure(
            lux::meta::entity_id entity
        )
        {
            auto [failure, inserted] = failed_.try_emplace(
                entity,
                FailRecord{false, false, false, 0}
            );
            (void)inserted;
            return failure->second;
        }

        /// 「实体离开了本子系统关心的组件集合」——**实体本身可能还活着**。
        /// 只记账;真正的 `remove<Live>` 在 `update()` 里做(CLAUDE.md 规矩一)。
        void onLeave(lux::meta::entity_id e)
        {
            (void)create_requests_.abandon(e);
            to_unbind_.push_back(e);
            failed_.erase(e);   // G-05 的失败记录同样按离场清理，保持有界
        }

        void drainCreateCompletions(lux::meta::EntityRegistry& registry)
        {
            create_requests_.drain(
                [this, &registry](auto completion)
                {
                    const auto e = completion.key;
                    const auto& reply = completion.reply;
                    const bool member_now =
                        inComponentView<Image2DComponent>(
                            registry, e, Require{}, Exclude{});
                    const bool owner_alive =
                        !completion.abandoned && member_now &&
                        !registry.all_of<Live>(e);
                    const bool succeeded =
                        !completion.dispatch_failed &&
                        reply.status == lux::render::ECanvas2DCreateStatus::Ok &&
                        !reply.handle.isNull();

                    if (!succeeded)
                    {
                        // Even a defensive non-null failure handle is treated as
                        // owned: reap it instead of relying on whole-scene teardown.
                        if (!reply.handle.isNull())
                            leaving_.push_back(reply.handle);
                        if (owner_alive)
                        {
                            auto& failure = rememberFailure(e);
                            if (completion.dispatch_failed)
                            {
                                const auto recovery = failure.dispatch_reported
                                    ? renderBridgeFailureRecovery(completion.error)
                                    : reportRenderBridgeFailure(
                                          "Image2DSubsystem",
                                          "add image",
                                          completion.error
                                      );
                                failure.dispatch_reported = true;
                                failure.permanent = recovery !=
                                    lux::render::ERecovery::Retryable;
                                failure.retry_in = failure.permanent
                                    ? 0
                                    : kTransientRetryDrives;
                                return;
                            }

                            const bool malformed_success =
                                reply.status ==
                                    lux::render::ECanvas2DCreateStatus::Ok &&
                                reply.handle.isNull();
                            const bool transient =
                                reply.status ==
                                    lux::render::ECanvas2DCreateStatus::CapacityExhausted;
                            failure.permanent = !transient;
                            failure.retry_in = transient
                                ? kTransientRetryDrives
                                : 0;
                            if (!failure.reply_reported)
                            {
                                if (malformed_success)
                                {
                                    diagnoseRenderBridge(
                                        "[Image2DSubsystem] add image returned success "
                                        "with a null handle; creation is latched until "
                                        "scene rebuild"
                                    );
                                }
                                else
                                {
                                    diagnoseRenderBridge(
                                        "[Image2DSubsystem] add image was refused "
                                        "(status {}); {}",
                                        static_cast<unsigned>(reply.status),
                                        transient
                                            ? "retrying after bounded backoff"
                                            : "creation is latched until scene rebuild"
                                    );
                                }
                                failure.reply_reported = true;
                            }
                        }
                        else if (member_now)
                        {
                            changes_.mark(e);
                        }
                        return;
                    }

                    if (!owner_alive)
                    {
                        leaving_.push_back(reply.handle);
                        if (member_now)
                            changes_.mark(e);
                        return;
                    }

                    failed_.erase(e);
                    completion.context.handle = reply.handle;
                    registry.emplace<Live>(e, std::move(completion.context));
                }
            );
        }

        /// **归还路径的唯一入口**(与 MeshInstance 同款,理由见那边的 onStateDestroyed):
        /// 锚点必须是**状态组件自己的** on_destroy —— `registry.destroy` 清各池的
        /// 顺序不保证,挂在别的组件上可能读到已经清掉的池。
        void onStateDestroyed(
            lux::meta::EntityRegistryBase& reg,
            lux::meta::entity_id e)
        {
            leaving_.push_back(reg.get<Live>(e).handle);
        }

        void attachStateSignal(lux::meta::EntityRegistry& reg)
        {
            if (reg_ == &reg) return;
            detachStateSignal();
            reg_ = &reg;
            reg.on_destroy<Live>().connect<&Image2DSubsystem::onStateDestroyed>(*this);
        }
        void detachStateSignal()
        {
            if (!reg_) return;
            reg_->on_destroy<Live>().disconnect<&Image2DSubsystem::onStateDestroyed>(*this);
            reg_ = nullptr;
        }

        /// 什么算「这个实体需要重新处理」。逐条显式列出 —— 漏一条的后果是
        /// 那一类变化下游永远收不到,而它不报错。
        void attachChangeSources(lux::meta::EntityRegistry& reg)
        {
            changes_.attach(reg,
                static_cast<entt::id_type>(renderSubsystemType<Image2DSubsystem>().hash),
                [](auto& s)
            {
                s.template on_construct<Image2DComponent>();
                s.template on_update   <Image2DComponent>();      // uv / tint / priority / visible / group
                s.template on_construct<ResolvedTransform2DComponent>();
                s.template on_update   <ResolvedTransform2DComponent>();
                // 贴图解析结果:出现/变/消失都改 `tex`。
                s.template on_construct<TextureGpuCacheComponent>();
                s.template on_update   <TextureGpuCacheComponent>();
                s.template on_destroy  <TextureGpuCacheComponent>();
                // A2-03 的两个可选修饰器:增删都改烘焙结果。
                s.template on_construct<Parallax2DComponent>();
                s.template on_update   <Parallax2DComponent>();
                s.template on_destroy  <Parallax2DComponent>();
                s.template on_construct<YSort2DComponent>();
                s.template on_update   <YSort2DComponent>();
                s.template on_destroy  <YSort2DComponent>();
                // 自己的状态组件:construct = 刚变 live(要发首次可见状态);
                // destroy = 离场/拆掉 → 下一轮重新考虑首见。
                s.template on_construct<Live>();
                s.template on_destroy  <Live>();
            });
        }
    };

} // namespace lux::ecs
