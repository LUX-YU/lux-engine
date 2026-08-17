#include <lux/engine/ecs/render/subsystems/CameraViewSubsystem.hpp>

#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>   // diagnoseRenderBridge
#include <lux/engine/ecs/render/components/ViewPresentComponent.hpp>
#include <lux/engine/ecs/render/components/RenderViewBindingComponent.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>     // formatRenderError

#include <unordered_map>
#include <vector>

namespace lux::ecs
{
    /// 本系统的全部延迟动作。**值命令**,不是闭包 —— 载荷至多一个实体 id,生产者
    /// 由槽位代次认。`apply` 的实现放在 `Impl` 定义之后(它们要碰 Impl 的状态)。
    struct CameraViewSubsystem::Commands
    {
        /// 有实体想出图了(on_construct,或连信号时折入的存量)。
        struct AddViewRequested
        {
            using Producer = CameraViewSubsystem;
            entt::entity entity{};
            [[nodiscard]] std::size_t registryPublicationBytes()
                const noexcept { return 0u; }
            void prepareRegistryPublication(
                lux::meta::EntityRegistry&) const noexcept {}
            void apply(lux::meta::EntityRegistry&, CameraViewSubsystem&) const;
        };

        /// 出图意图变了(target / order)。Android 的 surface 重建走这条。
        struct LayerRefreshRequested
        {
            using Producer = CameraViewSubsystem;
            entt::entity entity{};
            [[nodiscard]] std::size_t registryPublicationBytes()
                const noexcept { return 0u; }
            void prepareRegistryPublication(
                lux::meta::EntityRegistry&) const noexcept {}
            void apply(lux::meta::EntityRegistry&, CameraViewSubsystem&) const;
        };

        /// 摘掉了「要出图」→ 把绑定也摘掉;真正的 removeView 由绑定的 lease 析构发。
        struct ViewBindingDropRequested
        {
            using Producer = CameraViewSubsystem;
            entt::entity entity{};
            [[nodiscard]] std::size_t registryPublicationBytes()
                const noexcept { return 0u; }
            void prepareRegistryPublication(
                lux::meta::EntityRegistry&) const noexcept {}
            void apply(lux::meta::EntityRegistry&, CameraViewSubsystem&) const;
        };

        /// 绑定没了 —— 桥不再面向任何 view。
        struct ActiveViewCleared
        {
            using Producer = CameraViewSubsystem;
            [[nodiscard]] std::size_t registryPublicationBytes()
                const noexcept { return 0u; }
            void prepareRegistryPublication(
                lux::meta::EntityRegistry&) const noexcept {}
            void apply(lux::meta::EntityRegistry&, CameraViewSubsystem&) const;
        };
    };

    struct CameraViewSubsystem::Impl final
    {
        lux::render::RenderControlSession*  control{nullptr};
        lux::render::RenderSceneId          scene_id{};
        /// 出图 view 的场景资源。此前这里是一个 `RenderSystem*` —— 系统间的
        /// 长期裸指针,只为在 view 落地时反向通知它。现在两边都只认识资源。
        ActiveRenderView*                   active_view{nullptr};

        /// 往本节点命令分片写的凭据,`onAdded` 收下。观察者拿它入队 —— 它是可复制
        /// 的小 POD,过期(系统被摘除)只是写进去的命令在 barrier 被判掉,不是悬垂。
        EcsCommandWriter                    commands{};

        /// 只作主线程身份比较；World 按契约晚于系统析构。
        lux::meta::EntityRegistryBase* attached{nullptr};
        entt::scoped_connection present_constructed_connection{};
        entt::scoped_connection present_updated_connection{};
        entt::scoped_connection present_destroyed_connection{};
        entt::scoped_connection binding_destroyed_connection{};

        /// 在途的 addView：实体 → 请求。回复落地才变成 RenderViewBindingComponent。
        /// 这就是规矩三（异步就绪不走观察者）的落点：观察者只记「这个实体要 view」，
        /// 句柄由轮询装上。
        std::unordered_map<entt::entity,
                           lux::render::RenderRequest<lux::render::ViewCreatedReply>> pending;

        struct LayerBinding final
        {
            lux::render::RenderTargetId target{};
            std::uint32_t order{0u};
        };
        std::unordered_map<entt::entity, LayerBinding> layers;
        bool closing{false};

        // ── 观察者。四条都只入队，不就地改世界（规矩一） ──────────────────
        //
        // 入的是**值命令**。此前入的是 `[weak_ptr<Impl>, entity]` 闭包,而那个
        // weak_ptr 存在的唯一理由就是「队列可能比系统活得久」—— 现在生产者由槽位
        // 代次认,系统没了 barrier 直接判掉,兜底逻辑不必写在每个观察者里。

        template <class Cmd>
        void enqueue(const Cmd& cmd, const char* what)
        {
            if (commands.push(cmd)) return;
            // 唯一可能:系统还没被 addSystem 收下(没有 writer)。响亮说出来 ——
            // 静默丢弃的后果是「这台相机永远没有 view」,症状是黑屏零报错。
            diagnoseRenderBridge(
                "[CameraViewSubsystem] {} command dropped — the subsystem has no "
                "command writer (not installed into a Schedule?)", what);
        }

        void onPresentConstructed(
            lux::meta::EntityRegistryBase&,
            entt::entity e)
        {
            enqueue(Commands::AddViewRequested{e}, "add-view");
        }

        void onPresentUpdated(
            lux::meta::EntityRegistryBase&,
            entt::entity e)
        {
            // target / order 变了（Android surface 重建走这条）。extent 变化只影响
            // aspect，不需要重建 view —— view 的渲染尺寸随 target 派生。
            enqueue(Commands::LayerRefreshRequested{e}, "layer-refresh");
        }

        void onPresentDestroyed(
            lux::meta::EntityRegistryBase&,
            entt::entity e)
        {
            // 摘掉「要出图」→ 把绑定也摘掉，真正的 removeView 由绑定的 on_destroy 发。
            // 实体整体销毁时这条是多余的（绑定自己也会收到 on_destroy），所以命令里
            // 必须查 valid：那时实体已经没了。
            enqueue(Commands::ViewBindingDropRequested{e}, "drop-binding");
        }

        void onBindingDestroyed(
            lux::meta::EntityRegistryBase&,
            entt::entity)
        {
            // Resource return is now the component's move-only lease destructor.
            // The observer only updates the non-owning upload bridge at a safe
            // point; it no longer copies a handle into a manual remove request.
            enqueue(Commands::ActiveViewCleared{}, "clear-active-view");
        }

        // ── 延迟命令的实际动作 ────────────────────────────────────────────

        void issueAddView(lux::meta::EntityRegistry& reg, entt::entity e)
        {
            if (!reg.valid(e)) return;
            const auto* present = reg.try_get<ViewPresentComponent>(e);
            if (!present) return;                 // 同一帧内又被摘掉了
            if (pending.contains(e)) return;      // 已经在途
            if (reg.all_of<RenderViewBindingComponent>(e)) return;   // 已经有 view

            pending.emplace(e, control->addView(scene_id, present->extent, "CameraView"));
        }

        void relayer(lux::meta::EntityRegistry& reg, entt::entity e)
        {
            if (!reg.valid(e)) return;
            const auto* present = reg.try_get<ViewPresentComponent>(e);
            const auto* bind    = reg.try_get<RenderViewBindingComponent>(e);
            if (!present || !bind || !bind->view().isValid()) return;
            if (const auto old = layers.find(e); old != layers.end())
            {
                if (old->second.target != present->target ||
                    old->second.order != present->order)
                {
                    control->removeLayer(
                        old->second.target,
                        old->second.order);
                }
            }
            control->setLayer(present->target, present->order, scene_id, bind->view());
            layers.insert_or_assign(
                e,
                LayerBinding{present->target, present->order});
        }

        void dropBinding(lux::meta::EntityRegistry& reg, entt::entity e)
        {
            if (const auto old = layers.find(e); old != layers.end())
            {
                control->removeLayer(old->second.target, old->second.order);
                layers.erase(old);
            }
            if (reg.valid(e) && reg.all_of<RenderViewBindingComponent>(e))
                reg.remove<RenderViewBindingComponent>(e);
        }

        /// 回复落地：装上句柄、合成到 target、告诉桥。首帧矩阵由维度所属的
        /// Camera2D/3DUploadSubsystem::settle 补推，公共 view 生命周期不认识相机形状。
        void applyReady(lux::meta::EntityRegistry& reg)
        {
            if (pending.empty()) return;

            std::vector<entt::entity> done;
            for (auto& [e, req] : pending)
            {
                if (!req.isReady()) continue;
                done.push_back(e);

                const auto view = req.tryResult()->get().view;
                if (req.failed() || !view.isValid())
                {
                    // ★ 必须说话。pending.erase 之后不会重发(issueAddView 只由观察者/
                    //   attach 折入触发)——这台相机从此永远没有 view,宿主 target 上
                    //   零层,症状是黑屏零报错。这里是唯一能说出「为什么」的时刻。
                    const auto reason = req.failed() ? req.error() : req.tryResult()->get().error;
                    diagnoseRenderBridge(
                        "[CameraViewSubsystem] addView {} for camera entity {} — this "
                        "camera will never present (no view, no layer): {}",
                        req.failed() ? "failed in dispatch" : "was rejected",
                        static_cast<unsigned>(entt::to_integral(e)),
                        lux::render::formatRenderError(
                            lux::render::renderErrorRegistry(), reason).c_str());
                    continue;
                }
                auto lease = control->adoptView(
                    scene_id, view,
                    [view](const lux::render::GenericOkReply& ok)
                    {
                        if (ok.code != 0)
                            diagnoseRenderBridge(
                                "[CameraViewSubsystem] view lease release(view {}) "
                                "was rejected: {}",
                                static_cast<unsigned>(view.index),
                                lux::render::formatRenderError(
                                    lux::render::renderErrorRegistry(),
                                    ok.error).c_str());
                    });
                if (!reg.valid(e) || !reg.all_of<ViewPresentComponent>(e))
                {
                    // Entity or intent disappeared while addView was in flight.
                    // The freshly-adopted lease falls out of scope and queues
                    // exactly one compensating remove; no orphan survives.
                    continue;
                }

                reg.emplace_or_replace<RenderViewBindingComponent>(
                    e, std::move(lease));

                if (const auto* present = reg.try_get<ViewPresentComponent>(e))
                {
                    control->setLayer(present->target, present->order, scene_id, view);
                    layers.insert_or_assign(
                        e,
                        LayerBinding{present->target, present->order});
                }

                // 桥面向新 view。setView 会自增代次，MeshInstanceSubsystem 据此把已有实例
                // 对新 view 重发一遍可见性 —— 少了这一步，换相机之后场景全空。
                active_view->setView(view);

            }
            for (auto e : done) pending.erase(e);
        }

        // ── 信号连接 ──────────────────────────────────────────────────────

        void attach(lux::meta::EntityRegistryBase& r)
        {
            if (attached == &r) return;
            detach();
            attached = &r;
            present_constructed_connection =
                r.on_construct<ViewPresentComponent>()
                    .connect<&Impl::onPresentConstructed>(*this);
            present_updated_connection =
                r.on_update<ViewPresentComponent>()
                    .connect<&Impl::onPresentUpdated>(*this);
            present_destroyed_connection =
                r.on_destroy<ViewPresentComponent>()
                    .connect<&Impl::onPresentDestroyed>(*this);
            binding_destroyed_connection =
                r.on_destroy<RenderViewBindingComponent>()
                    .connect<&Impl::onBindingDestroyed>(*this);

            // ★ 把**已经存在**的 ViewPresentComponent 折进来。
            //
            //   信号只对连接之后发生的事说话。宿主的自然写法是「建相机 → 挂组件 →
            //   settleViewCreation()」,而连接就发生在 settle 里 —— 组件早在那之前
            //   就 emplace 了,on_construct 永远不会为它触发,于是**一个 view 都不会
            //   建**:编辑器照常启动、干净退出、零报错,主场景却不渲染。
            //   (这不是假设,是本步骤实机验证撞出来的。)
            //
            //   与 `ensureHierarchyIndex` 同一处方:连信号 + 折入存量,两件一起做,
            //   「谁先谁后」才不再是调用方要操心的事。
            for (auto e : r.view<ViewPresentComponent>())
                enqueue(Commands::AddViewRequested{e}, "add-view(backfill)");
        }

        void detach()
        {
            if (!attached) return;
            present_constructed_connection.release();
            present_updated_connection.release();
            present_destroyed_connection.release();
            binding_destroyed_connection.release();
            attached = nullptr;
        }
    };

    // ── 命令的实现。放在 Impl 之后:它们要碰 Impl 的状态 ──────────────────────

    void CameraViewSubsystem::Commands::AddViewRequested::apply(
        lux::meta::EntityRegistry& reg, CameraViewSubsystem& sys) const
    {
        if (sys.impl_->closing)
            return;
        sys.impl_->issueAddView(reg, entity);
    }

    void CameraViewSubsystem::Commands::LayerRefreshRequested::apply(
        lux::meta::EntityRegistry& reg, CameraViewSubsystem& sys) const
    {
        if (sys.impl_->closing)
            return;
        sys.impl_->relayer(reg, entity);
    }

    void CameraViewSubsystem::Commands::ViewBindingDropRequested::apply(
        lux::meta::EntityRegistry& reg, CameraViewSubsystem& sys) const
    {
        if (sys.impl_->closing)
            return;
        // 实体整体销毁时这条是多余的(绑定自己也收到 on_destroy),所以必须查 valid:
        // 那时实体已经没了。
        sys.impl_->dropBinding(reg, entity);
    }

    void CameraViewSubsystem::Commands::ActiveViewCleared::apply(
        lux::meta::EntityRegistry&, CameraViewSubsystem& sys) const
    {
        if (sys.impl_->closing)
            return;
        // 桥不再面向这个 view。下一台相机的 view 落地时会 setView 到新的；
        // 空句柄让 MeshInstanceSubsystem 在此期间不发可见性命令。
        sys.impl_->active_view->setView({});
    }

    // -------------------------------------------------------------------------
    CameraViewSubsystem::CameraViewSubsystem()
        : impl_(std::make_unique<Impl>())
    {
    }

    CameraViewSubsystem::~CameraViewSubsystem() = default;

    void CameraViewSubsystem::onAdded(
        const lux::ecs::SystemSetupContext& setup)
    {
        // 先收下 writer,再连信号 —— 折入存量要往分片里写,顺序反了就静默丢一批。
        impl_->closing = false;
        impl_->commands = setup.commands();
        impl_->attach(setup.registry());
    }

    void CameraViewSubsystem::onRemoved(
        const lux::ecs::SystemRemovalContext&)
    {
        impl_->detach();
    }

    void CameraViewSubsystem::prepare(
        RenderSubsystemContext& context) noexcept
    {
        auto& render = context.render();
        impl_->control = &render.control();
        impl_->scene_id = render.scene();
        impl_->active_view = &context.activeView();
    }

    void CameraViewSubsystem::update(RenderSubsystemContext& context)
    {
        auto& registry = context.registry();

        // ★ 这里此前有两件事,都搬走了:
        //   · `attach(registry)` —— 挪到 `onAdded`。装配期就连信号 + 折入存量,
        //     不必等第一次 update(那是 ISystem 头注释点名的懒初始化 ensure 形状)。
        //   · `flush` —— 排空归 `Schedule::applyCommandBarrier()`,在 tick 末尾,
        //     全项目只有那一个 apply 点。本系统不再自己开一个。

        // 规矩二/三：结构性转换用观察者，异步就绪用帧内轮询。
        impl_->applyReady(registry);

    }

    void CameraViewSubsystem::settle(RenderSubsystemContext& context)
    {
        auto& registry = context.registry();
        // attach 已在 onAdded 做过;把 addView 请求发出去的那次排空归调用方
        // (`SceneRuntime::settleViewCreation` 先调 `Schedule::applyCommandBarrier()`)
        // —— 本系统不再自己开第二个 apply 点。
        if (impl_->pending.empty()) return;

        // 裸指针而非 shared_ptr:这是一次**同线程的阻塞等待**,本系统在整个调用期间
        // 必然活着(调用它的正是拥有它的那条装配路径)。
        auto* const state = impl_.get();
        const auto all_ready = [state] {
            for (auto& [e, req] : state->pending)
                if (!req.isReady()) return false;
            return true;
        };
        if (!impl_->control->awaitAllReady(all_ready))
            return;   // 通道已停：宿主正在退出，装不装 view 都不再重要

        impl_->applyReady(registry);
    }

    void CameraViewSubsystem::close(
        RenderSubsystemContext& context) noexcept
    {
        // A Scene may close while it is still in kPhaseSceneLoading. In that
        // case the ordinary render update (and therefore prepare()) has never
        // run, but close still owns a complete context. Bind the two borrowed
        // resources here as well so observer commands emitted by component
        // removal can be drained by the close barrier safely.
        impl_->control = &context.render().control();
        impl_->active_view = &context.activeView();
        impl_->closing = true;
        impl_->detach();
        auto& registry = context.registry();
        for (const auto& [_, layer] : impl_->layers)
        {
            impl_->control->removeLayer(layer.target, layer.order);
        }
        impl_->layers.clear();
        std::vector<entt::entity> bound;
        for (const auto e : registry.view<RenderViewBindingComponent>())
            bound.push_back(e);

        for (const auto e : bound)
        {
            if (!registry.valid(e) || !registry.all_of<RenderViewBindingComponent>(e))
                continue;
            (void)registry.get<RenderViewBindingComponent>(e).close();
            registry.remove<RenderViewBindingComponent>(e);
        }

        impl_->pending.clear();
        impl_->active_view->setView({});
    }

} // namespace lux::ecs
