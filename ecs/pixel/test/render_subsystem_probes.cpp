// ============================================================================
//  render_subsystem_probes.cpp — 渲染桥不变量的**常驻回归网**。
//
//  最初是「落地转绿后删除」的开发期探针（①-⑤），但此后每条工作线都把自己
//  「测试与实机日志都抓不到」的不变量钉在这里,它已是这些语义唯一的自动化
//  覆盖 —— 按能力占位保留,不再删除。HeadlessBridgeFixture 是它的地基。
//
//  为什么非探不可:这里的每一条错法都不报错 —— 漏还资源是「那盏灯一直亮着」,
//  解析序漂移是「叠层画错」,共享缓存回归是「另一个世界黑屏」;退出码 0,
//  stderr 逐行相同。
//
//  目录:
//    ①-④ PooledSlotSubsystem 离场观察者(销毁/摘 Require/在途回复/重进场)
//    ⑤   特性参数子系统吃 cache 组件(读得到就发命令)
//    ⑥   MeshInstanceSubsystem 实体销毁离场
//    ⑦   高亮实体标签化
//    ⑧   view 代次驱动的实例可见性重发
//    ⑨   资产引用计数端到端(AssetRef 票据语义:拷贝/移动/归零广播)
//    ⑩   attach 解析器纯函数(根+闭包+稳定拓扑序;⑩-f 是向后兼容承诺)
//    ⑪   进程域共享缓存跨世界语义(一份上传/别的世界还在用不销毁/
//        releaseRefs 只还本场景的票)——不探则回归完全静默
//    ⑬   内容变更(replaceAsset)→ GPU 副本重建的热更新链(bump→广播→强制
//        销毁→缓存纪元→resolver 重查→重上传→组件句柄变→实例拆重建)
//    ⑭   Residency callback/ticket 逃逸 owner 后安全失效 + callback 内 close
// ============================================================================

#include "HeadlessBridgeFixture.hpp"

#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Camera2DUploadSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Image2DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/PixelField2DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Tilemap2DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/MeshInstanceSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/LightSubsystems.hpp>   // PointLightSubsystem
#include <lux/engine/ecs/render/subsystems/3d/SkyboxSubsystem.hpp>   // SkyboxSubsystem
#include <lux/engine/ecs/render/components/TextureGpuCacheComponent.hpp>
#include <lux/engine/ecs/render/components/MeshGpuCacheComponent.hpp>
#include <lux/engine/ecs/render/subsystems/3d/MeshSubsystems.hpp>    // MeshSubsystem
#include <lux/engine/ecs/render/components/HighlightedComponent.hpp>
#include <lux/engine/ecs/render/components/AssetStreamingStateComponent.hpp>
#include <lux/engine/ecs/render/components/VisualTransitionComponent.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>  // ⑨:引用计数端到端(驻留胶水)
#include <lux/engine/ecs/render/RenderSystemBuilder.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/runtime/render/scene/ResidencyAssembly.hpp>           // 驻留三件套(T12 起探针走真装配)
#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>
#include <lux/engine/resource/asset/mesh/MeshSerDeser.hpp>             // MeshAsset(⑨ 要一份真资产)
#include <lux/engine/resource/asset/AssetEvents.hpp>              // 资产广播事件(批E)
#include <lux/engine/events/DomainEvents.hpp>                // 探针自建 bus+pump(批E)
#include <lux/engine/ecs/Registry.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/SystemPhase.hpp>

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    struct PlanA final : lux::ecs::IRenderSubsystem
    {
        explicit PlanA(std::vector<int>* trace = nullptr) noexcept
            : trace(trace) {}
        void update(lux::ecs::RenderSubsystemContext&) override
        {
            if (trace) trace->push_back(1);
        }
        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept override
        {
            static constexpr std::string_view kFeatures[]{"feature.z", "feature.a"};
            return kFeatures;
        }
        std::vector<int>* trace{};
    };

    struct PlanB final : lux::ecs::IRenderSubsystem
    {
        explicit PlanB(std::vector<int>* trace = nullptr) noexcept
            : trace(trace) {}
        [[nodiscard]] std::span<const lux::ecs::RenderSubsystemType>
        prerequisites() const noexcept override
        {
            static constexpr lux::ecs::RenderSubsystemType kRequired[]{
                lux::ecs::renderSubsystemType<PlanA>()};
            return kRequired;
        }
        void update(lux::ecs::RenderSubsystemContext&) override
        {
            if (trace) trace->push_back(2);
        }
        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept override
        {
            static constexpr std::string_view kFeatures[]{"feature.a", "feature.m"};
            return kFeatures;
        }
        std::vector<int>* trace{};
    };

    struct PlanC final : lux::ecs::IRenderSubsystem
    {
        explicit PlanC(std::vector<int>* trace = nullptr) noexcept
            : trace(trace) {}
        void update(lux::ecs::RenderSubsystemContext&) override
        {
            if (trace) trace->push_back(3);
        }
        std::vector<int>* trace{};
    };

    struct MissingPlanDependency final : lux::ecs::IRenderSubsystem
    {
        [[nodiscard]] std::span<const lux::ecs::RenderSubsystemType>
        prerequisites() const noexcept override
        {
            static constexpr lux::ecs::RenderSubsystemType kRequired[]{
                lux::ecs::renderSubsystemType<PlanA>()};
            return kRequired;
        }
        void update(lux::ecs::RenderSubsystemContext&) override {}
    };

    struct CyclePlanRight;
    struct CyclePlanLeft final : lux::ecs::IRenderSubsystem
    {
        [[nodiscard]] std::span<const lux::ecs::RenderSubsystemType>
        runsAfter() const noexcept override;
        void update(lux::ecs::RenderSubsystemContext&) override {}
    };
    struct CyclePlanRight final : lux::ecs::IRenderSubsystem
    {
        [[nodiscard]] std::span<const lux::ecs::RenderSubsystemType>
        runsAfter() const noexcept override
        {
            static constexpr lux::ecs::RenderSubsystemType kAfter[]{
                lux::ecs::renderSubsystemType<CyclePlanLeft>()};
            return kAfter;
        }
        void update(lux::ecs::RenderSubsystemContext&) override {}
    };
    std::span<const lux::ecs::RenderSubsystemType>
    CyclePlanLeft::runsAfter() const noexcept
    {
        static constexpr lux::ecs::RenderSubsystemType kAfter[]{
            lux::ecs::renderSubsystemType<CyclePlanRight>()};
        return kAfter;
    }

    struct DynamicPlanRoot final : lux::ecs::IRenderSubsystem
    {
        explicit DynamicPlanRoot(std::vector<int>* trace) noexcept
            : trace(trace)
        {}

        void update(lux::ecs::RenderSubsystemContext&) override
        {
            trace->push_back(4);
        }

        void onRemoved(const lux::ecs::SystemRemovalContext&) override
        {
            trace->push_back(-4);
        }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

        std::vector<int>* trace;
    };

    struct DynamicPlanLeaf final : lux::ecs::IRenderSubsystem
    {
        explicit DynamicPlanLeaf(std::vector<int>* trace) noexcept
            : trace(trace)
        {}

        [[nodiscard]] std::span<const lux::ecs::RenderSubsystemType>
        prerequisites() const noexcept override
        {
            static constexpr lux::ecs::RenderSubsystemType kRequired[]{
                lux::ecs::renderSubsystemType<DynamicPlanRoot>()};
            return kRequired;
        }

        void update(lux::ecs::RenderSubsystemContext&) override
        {
            trace->push_back(5);
        }

        void onRemoved(const lux::ecs::SystemRemovalContext&) override
        {
            trace->push_back(-5);
        }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

        std::vector<int>* trace;
    };

    template <class Subsystem>
    void drive(
        Subsystem&                         subsystem,
        lux::ecs::Registry&         registry,
        lux::ecs::SceneRenderBinding&      binding,
        lux::ecs::ActiveRenderView&        active_view,
        float                              dt
    )
    {
        lux::ecs::RenderSubsystemContext context{
            registry,
            {},
            binding,
            active_view,
            dt,
            0,
        };
        subsystem.prepare(context);
        subsystem.update(context);
    }

    int g_failures = 0;

    void check(bool ok, const char* what)
    {
        std::fprintf(stderr, "[%s] %s\n", ok ? " ok " : "FAIL", what);
        if (!ok) ++g_failures;
    }

    bool containsSubsystemType(
        std::span<const lux::ecs::RenderSubsystemType> types,
        lux::ecs::RenderSubsystemType expected
    ) noexcept
    {
        for (const auto type : types)
            if (type == expected)
                return true;
        return false;
    }

    /// 批E:资产广播的探针装配 —— 三队列退役后,探针自建 bus+pump,把账本
    /// 回调接成事件、缓存方法接成订阅(宿主装配的同构缩影;单测红利:零全局
    /// 状态)。drain() 用 drainUntilEmpty:一次「帧首派发」把批内级联(销毁
    /// 材质 → 贴图归零 → 再销毁)收敛干净 —— 旧 while-到-空排空的对应物。
    /// 析构收口账本回调:探针尾段(缓存/系统析构)的归零不再摸已死的 bus。
    struct AssetEventRig
    {
        lux::asset::AssetManager&      assets;
        lux::events::DomainEvents          bus;
        lux::events::EventPump&        pump;
        lux::events::SubscriptionGroup subs;

        AssetEventRig(lux::asset::AssetManager&        mgr,
                      lux::runtime::ResidencyAssembly& residency)
            : assets(mgr), pump(bus.createPump("probe"))
        {
            assets.setBroadcast({
                .on_unreferenced =
                    [this](const lux::asset::asset_id_t& id)
                    { bus.publish(lux::asset::AssetUnreferenced{id}); },
                .on_invalidated =
                    [this](const lux::asset::asset_id_t& id)
                    { bus.publish(lux::asset::AssetInvalidated{id}); },
                .on_content_changed =
                    [this](const lux::asset::asset_id_t& id, std::uint32_t rev)
                    { bus.publish(lux::asset::AssetContentChanged{id, rev}); },
                .on_registered =
                    [this](const lux::asset::asset_id_t& id)
                    { bus.publish(lux::asset::AssetRegistered{id}); },
            });
            // 订阅顺序镜像宿主装配(承载序):invalidated → content_changed →
            // unreferenced(泵按通道首订阅序派发);registered 无序要求。
            auto residency_events = residency.makeAssetEventCallbacks();
            subs.add(bus.subscribe<lux::asset::AssetInvalidated>(
                pump,
                [fn = std::move(residency_events.invalidated)](
                    const lux::asset::AssetInvalidated& e) mutable
                { fn(e.id); }));
            subs.add(bus.subscribe<lux::asset::AssetContentChanged>(
                pump,
                [fn = std::move(residency_events.content_changed)](
                    const lux::asset::AssetContentChanged& e) mutable
                { fn(e.id); }));
            subs.add(bus.subscribe<lux::asset::AssetUnreferenced>(
                pump,
                [fn = std::move(residency_events.unreferenced)](
                    const lux::asset::AssetUnreferenced& e) mutable
                { fn(e.id); }));
            subs.add(bus.subscribe<lux::asset::AssetRegistered>(
                pump,
                [fn = std::move(residency_events.registered)](
                    const lux::asset::AssetRegistered& e) mutable
                { fn(e.id); }));
        }

        ~AssetEventRig() { assets.setBroadcast({}); }

        void drain() { pump.drainUntilEmpty(); }
    };

    /// Each probe owns the same async service graph as a production host.
    /// 注入点接线 —— 与宿主装配同构缩影。
    struct ResidencyRig
    {
        using FailureSink =
            lux::runtime::ResidencyAssembly::FailureSink;

        lux::bridgetest::HeadlessBridgeFixture& fixture;
        lux::runtime::testing::AsyncTestServices async;
        lux::runtime::ResidencyAssembly residency;

        ResidencyRig(lux::bridgetest::HeadlessBridgeFixture& bridge,
                     lux::asset::AssetManager&               assets,
                     FailureSink failure_sink = {})
            : fixture(bridge)
            , async(
                  assets,
                  fixture.upload(),
                  fixture.sync(),
                  lux::exec::AsyncRuntimeConfig{
                      .blocking_io_threads = 1,
                      .background_cpu_concurrency = 1}
              )
            , residency(
                  fixture.control(),
                  async.uploadClient(),
                  assets,
                  fixture.features(),
                  async.assetClient(),
                  async.runtime(),
                  std::move(failure_sink)
              )
        {
            check(async.valid(),
                  "ResidencyRig async service graph assembled");
        }

        [[nodiscard]] lux::runtime::ResidencyCloseReport close()
        {
            // Production has a dedicated render consumer. This deterministic
            // fixture dispatches inline, so its blocking close needs a temporary
            // SPSC consumer or trySubmitFrame() would wait for itself forever.
            std::jthread render_consumer{
                [this](std::stop_token stop)
                {
                    const auto sync = fixture.sync();
                    std::stop_callback wake{
                        stop,
                        [sync]() noexcept
                        {
                            sync->notifyRequestStateChanged();
                        }};
                    while (!stop.stop_requested())
                    {
                        const auto observed = sync->work_epoch.load(
                            std::memory_order_acquire);
                        fixture.dispatch();
                        if (!stop.stop_requested() &&
                            sync->work_epoch.load(
                                std::memory_order_acquire) == observed)
                        {
                            sync->work_epoch.wait(
                                observed,
                                std::memory_order_acquire);
                        }
                    }
                }
            };
            const auto report =
                lux::runtime::testing::detail::closeResidency(
                    residency,
                    async.runtime());
            render_consumer.request_stop();
            render_consumer.join();
            return report;
        }

        ~ResidencyRig()
        {
            const auto report = close();
            check(report.terminal(),
                  "ResidencyRig RAII close reaches a terminal state");
        }

        [[nodiscard]] std::unique_ptr<lux::ecs::ResidencySubsystem>
        makeGlue(lux::asset::AssetManager& assets)
        {
            auto glue = std::make_unique<lux::ecs::ResidencySubsystem>(assets);
            glue->setCallbacks(residency.makeCallbacks());
            return glue;
        }

        [[nodiscard]] bool settleBridge()
        {
            const auto progress = [this]
            {
                fixture.dispatch();
                fixture.pump();
            };
            for (std::size_t turn = 0u; turn < 10000u; ++turn)
            {
                (void)async.settle(progress, 64u);
                if (!residency.hasInflight())
                    return async.settle(progress, 64u);
            }
            return false;
        }
    };

    template <int Id>
    struct PhaseProbe final : lux::ecs::ISystem
    {
        std::vector<int>* out;
        int               tag;

        PhaseProbe(std::vector<int>* output, int value)
            : out(output), tag(value)
        {
        }

        void update(const lux::ecs::SystemUpdateContext& /*ctx*/) override
        {
            out->push_back(tag);
        }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
    };

    template <int Id>
    struct OrderProbe final : lux::ecs::ISystem
    {
        std::vector<int>*            out;
        int                          tag;
        std::vector<lux::ecs::SystemType> after;
        std::vector<lux::ecs::SystemType> before;

        OrderProbe(std::vector<int>* output, int value)
            : out(output), tag(value)
        {
        }

        void update(const lux::ecs::SystemUpdateContext& /*ctx*/) override
        {
            out->push_back(tag);
        }

        std::span<const lux::ecs::SystemType> runsAfter() const noexcept override
        {
            return after;
        }

        std::span<const lux::ecs::SystemType> runsBefore() const noexcept override
        {
            return before;
        }

        AccessDeclaration accessDeclaration() const noexcept override
        {
            return {.resources = {}, .complete = true, .structural = false};
        }
    };

    struct BatchResourceA {};
    struct BatchResourceB {};

    template <int Id, class Resource, bool Writes>
    struct AccessProbe final : lux::ecs::ISystem
    {
        void update(const lux::ecs::SystemUpdateContext& /*ctx*/) override {}

        AccessDeclaration accessDeclaration() const noexcept override
        {
            static constexpr ResourceAccess kAccess[] = {
                Writes ? writes<Resource>() : reads<Resource>()};
            return {
                .resources = kAccess,
                .complete = true,
                .structural = false,
            };
        }
    };

    /// 一个挂了点光源 + 世界变换的实体（点光源的 Require 是变换：位置从那里取）。
    lux::ecs::Entity makeLight(lux::ecs::Registry& reg, float x)
    {
        const auto e = reg.create();
        reg.emplace<lux::ecs::PointLightComponent>(e);
        auto& wt = reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
        wt.linear = Eigen::Matrix3f::Identity();
        wt.position.x = x;
        return e;
    }

    /// 建一个装了 PointLight POOL 子系统的渲染系统，跑一帧让灯创建出来并落地。
    struct Harness
    {
        lux::bridgetest::HeadlessBridgeFixture fx;
        lux::ecs::Registry              reg;
        lux::ecs::ActiveRenderView             active_view{fx.view()};
        lux::ecs::SceneRenderBinding       render_ctx{
            fx.session(), fx.control(), {}, fx.scene()};
        /// 批 B3:池化槽位是普通节点,本探针手工驱动。
        lux::ecs::PointLightSubsystem          lights{};

        Harness()
        {
            fx.registerLightOps();
            render_ctx.setCatalog(fx.features());
            render_ctx.bindFeature("Skybox", lux::render::FeatureHandle{ 0u, 1u });
            lights.onAdded(lux::ecs::SystemSetupContext{reg, {}});
            fx.beginFrame();
        }

        ~Harness()
        {
            // 拆解契约：releaseRefs 要在帧开着的时候调（会发 destroy 命令）。
            fx.submit();
            fx.dispatch();
            fx.pump();
        }

        /// 一帧：update（构建器开着）→ 提交 → 假服务端派发+回复 → 客户端 pump。
        void frame()
        {
            drive(lights, reg, render_ctx, active_view, 1.f / 60.f);
            fx.roundTrip();
        }
    };
} // namespace

int main()
{
    // RenderSystemBuilder is a scene-assembly compiler, not another runtime
    // scheduler: reject malformed graphs once, then execute a frozen stable list.
    {
        lux::ecs::RenderSystemBuilder duplicates;
        check(duplicates.add(std::make_unique<PlanA>()).has_value(),
              "RenderPlan/duplicate: first type is accepted");
        const auto duplicate = duplicates.add(std::make_unique<PlanA>());
        check(!duplicate && duplicate.error().code ==
                  lux::ecs::ERenderAssemblyError::DuplicateType,
              "RenderPlan/duplicate: repeated type is rejected without RTTI");

        lux::ecs::RenderSystemBuilder missing_builder;
        check(missing_builder.add(
                  std::make_unique<MissingPlanDependency>()).has_value(),
              "RenderPlan/missing: candidate is staged");
        const auto missing = std::move(missing_builder).compile();
        check(!missing && missing.error().code ==
                  lux::ecs::ERenderAssemblyError::MissingPrerequisite,
              "RenderPlan/missing: hard prerequisite fails compilation");

        lux::ecs::RenderSystemBuilder cycle_builder;
        (void)cycle_builder.add(std::make_unique<CyclePlanLeft>());
        (void)cycle_builder.add(std::make_unique<CyclePlanRight>());
        const auto cycle = std::move(cycle_builder).compile();
        check(!cycle && cycle.error().code ==
                  lux::ecs::ERenderAssemblyError::TopologyCycle,
              "RenderPlan/cycle: internal dependency cycle is rejected");

        std::vector<int> trace;
        lux::ecs::RenderSystemBuilder builder;
        (void)builder.add(std::make_unique<PlanB>(&trace));
        (void)builder.add(std::make_unique<PlanC>(&trace));
        (void)builder.add(std::make_unique<PlanA>(&trace));
        auto plan = std::move(builder).compile();
        check(plan.has_value(), "RenderPlan/stable: valid graph compiles");
        if (plan)
        {
            check(std::vector<std::string_view>(plan->features().begin(),
                                                plan->features().end()) ==
                      std::vector<std::string_view>{
                          "feature.a", "feature.m", "feature.z"},
                  "RenderPlan/features: feature roots are sorted and unique");

            lux::bridgetest::HeadlessBridgeFixture fixture;
            lux::ecs::World world;
            lux::ecs::Schedule schedule{world};
            auto render_system = std::make_unique<lux::ecs::RenderSystem>(
                    fixture.session(), fixture.control(),
                    lux::render::RenderUploadClient{},
                    fixture.control().adoptScene(fixture.scene()),
                    std::move(*plan));
            auto* render_system_ptr = render_system.get();
            auto installed = schedule.addSystem(
                std::move(render_system),
                lux::ecs::kPhaseRender);
            check(installed.has_value(),
                  "RenderPlan/install: one top-level RenderSystem is installed");
            fixture.beginFrame();
            schedule.tick(0.0f);
            check(trace == std::vector<int>{3, 1, 2},
                  "RenderPlan/stable: ready nodes keep registration order around dependencies");

            lux::ecs::RenderSubsystemMutationBatch mutation;
            check(mutation.add(
                      std::make_unique<DynamicPlanLeaf>(&trace)).has_value(),
                  "RenderPlan/mutation: dependent can be staged before its prerequisite");
            check(mutation.add(
                      std::make_unique<DynamicPlanRoot>(&trace)).has_value(),
                  "RenderPlan/mutation: prerequisite joins the same atomic batch");
            auto dynamic = render_system_ptr->installSubsystemBatch(
                std::move(mutation));
            check(dynamic.has_value(),
                  "RenderPlan/mutation: a complete batch installs atomically");
            trace.clear();
            schedule.tick(0.0f);
            check(trace == std::vector<int>{3, 1, 2, 4, 5},
                  "RenderPlan/mutation: hot plan remains one stable topological pointer walk");

            if (dynamic)
            {
                trace.clear();
                auto removed = render_system_ptr->removeSubsystemBatch(
                    std::move(*dynamic));
                check(removed.has_value(),
                      "RenderPlan/mutation: a complete batch removes atomically");
                check(trace == std::vector<int>{-5, -4},
                      "RenderPlan/mutation: teardown follows reverse execution order");
                trace.clear();
                schedule.tick(0.0f);
                check(trace == std::vector<int>{3, 1, 2},
                      "RenderPlan/mutation: retired slots leave no hot-path entry");
            }
        }
        auto frozen_add = builder.add(std::make_unique<PlanC>());
        check(!frozen_add && frozen_add.error().code ==
                  lux::ecs::ERenderAssemblyError::BuilderFrozen,
              "RenderPlan/frozen: compiled builder rejects runtime mutation");
    }

    // ── ⓪ 渲染节点的数据依赖必须由类型边表达 ─────────────────────────────
    // 包安装顺序只负责构造，不能再暗中兼任帧内执行顺序。这里直接钉住消费
    // 关系，防止以后拆包、插入新节点或并行编译 schedule 时重新依赖偶然顺序。
    {
        lux::bridgetest::HeadlessBridgeFixture fx;
        lux::ecs::SceneRenderBinding render_ctx{
            fx.session(), fx.control(), {}, fx.scene()};
        lux::ecs::ActiveRenderView active_view{fx.view()};

        lux::ecs::Image2DSubsystem image{};
        lux::ecs::Tilemap2DSubsystem tilemap{nullptr};
        lux::ecs::PixelField2DSubsystem pixels{nullptr};
        lux::ecs::MeshSubsystem meshes{};
        lux::ecs::SkyboxSubsystem skybox{};

        const auto residency =
            lux::ecs::renderSubsystemType<lux::ecs::ResidencySubsystem>();
        const auto camera2d =
            lux::ecs::renderSubsystemType<lux::ecs::Camera2DUploadSubsystem>();

        check(containsSubsystemType(image.runsAfter(), residency) &&
                  containsSubsystemType(image.runsAfter(), camera2d),
              "⓪-a：Image2D 显式依赖 Residency + Camera2DUpload");
        check(containsSubsystemType(tilemap.runsAfter(), residency) &&
                  containsSubsystemType(tilemap.runsAfter(), camera2d),
              "⓪-b：Tilemap2D 显式依赖 Residency + Camera2DUpload");
        check(containsSubsystemType(pixels.runsAfter(), camera2d),
              "⓪-c：PixelField2D 显式依赖 Camera2DUpload");
        check(containsSubsystemType(meshes.runsAfter(), residency),
              "⓪-d：Mesh 实例显式依赖 Residency");
        check(containsSubsystemType(skybox.runsAfter(), residency),
              "⓪-e：Skybox 参数显式依赖 Residency");
    }

    // ── ① 销毁实体 → 资源被还 ────────────────────────────────────────────
    {
        Harness h;
        const auto e = makeLight(h.reg, 1.0f);
        h.frame();                                   // create 发出
        h.frame();                                   // 回复已落地 → live
        check(h.fx.recorder().created_lights.size() == 1, "①-前置：灯建起来了");
        const auto handle = h.fx.recorder().created_lights.at(0);

        h.reg.destroy(e);                            // ← 观察者在这一刻记账
        check(h.fx.recorder().destroyed_lights.empty(),
              "①-a：观察者内**没有**就地发命令（那时构建器没开，会被静默丢掉）");

        h.frame();                                   // tick 开头排空离场队列
        check(h.fx.recorder().destroyed_lights.size() == 1, "①-b：DestroyLight 发出了一次");
        check(!h.fx.recorder().destroyed_lights.empty() &&
              h.fx.recorder().destroyed_lights.at(0) == handle, "①-c：还的是那一个句柄");

        h.frame();
        check(h.fx.recorder().destroyed_lights.size() == 1, "①-d：不会重复还");
    }

    // ── ①-e 显式视觉过渡由 Render owner 渐入/渐出 ───────────────────────
    {
        Harness h;
        const auto e = makeLight(h.reg, 1.5f);
        h.reg.emplace<lux::ecs::VisualTransitionComponent>(
            e,
            lux::ecs::VisualTransitionComponent{0.35f, 0x9f4f5af1u});
        h.frame();
        h.frame();
        check(h.fx.recorder().count("CreateLight") == 1,
              "①-e 前置：带显式视觉过渡的 Point Light 已创建");
        if (h.fx.recorder().count("CreateLight") == 1)
        {
            const auto create = h.fx.recorder().payload<
                lux::render::CreateLightPayload>("CreateLight", 0);
            check(create.transition_milliseconds == 350u,
                  "①-e：Point Light 创建边沿使用组件声明的视觉时窗");
        }

        h.reg.destroy(e);
        h.frame();
        check(h.fx.recorder().count("DestroyLight") == 1,
              "①-f：Point Light 离场仍立即离开 Registry");
        if (h.fx.recorder().count("DestroyLight") == 1)
        {
            const auto destroy = h.fx.recorder().payload<
                lux::render::DestroyLightPayload>("DestroyLight", 0);
            check(destroy.transition_milliseconds == 350u,
                  "①-g：最终释放由 Render owner 在强度渐出后完成");
        }
    }

    // ── ② 摘掉 Require（组件还在，但离开了视图）────────────────────────────
    {
        Harness h;
        const auto e = makeLight(h.reg, 2.0f);
        h.frame();
        h.frame();
        check(h.fx.recorder().created_lights.size() == 1, "②-前置：灯建起来了");

        h.reg.remove<lux::ecs::ResolvedTransform3DComponent>(e);
        h.frame();
        check(h.fx.recorder().destroyed_lights.size() == 1,
              "②：丢了 Require 也算离场（漏连 on_destroy<Require> 就是这里静默泄漏）");
    }

    // ── ③ 离场时 create 还在途（坑①）─────────────────────────────────────
    {
        Harness h;
        const auto e = makeLight(h.reg, 3.0f);
        drive(
            h.lights,
            h.reg,
            h.render_ctx,
            h.active_view,
            1.0f / 60.0f
        );
        h.fx.submit();
        h.fx.dispatch();                             // 服务端建好了、回复排着队
        h.reg.destroy(e);                            // ★ 回复到达**之前**销毁
        h.fx.pump();                                 // 回复落地
        h.fx.beginFrame();

        check(h.fx.recorder().created_lights.size() == 1, "③-前置：服务端确实建了一个");
        h.frame();
        check(h.fx.recorder().destroyed_lights.size() == 1,
              "③：在途期间离场的，回复落地后照样还（不记这一笔=永久泄漏）");
    }

    // ── ④ 离场后又回来（坑②）────────────────────────────────────────────
    {
        Harness h;
        const auto e = makeLight(h.reg, 4.0f);
        h.frame();
        h.frame();
        const auto first = h.fx.recorder().created_lights.at(0);

        h.reg.remove<lux::ecs::PointLightComponent>(e);   // 离场
        h.reg.emplace<lux::ecs::PointLightComponent>(e);  // 同一帧内又回来
        h.frame();
        h.frame();

        check(h.fx.recorder().destroyed_lights.size() == 1, "④-a：旧句柄还掉了");
        check(h.fx.recorder().created_lights.size() == 2, "④-b：新句柄建起来了");
        check(h.fx.recorder().created_lights.size() == 2 &&
              h.fx.recorder().created_lights.at(1) != first, "④-c：确实是新的一个");
    }

    // ── ⑤ PARAM 子系统改吃 cache 组件（阶段 4）────────────────────────────
    //
    //  天空盒此前自己调 `ensureTexture` 推进异步上传状态机；现在只 `try_get`
    //  资源子系统写好的 `TextureGpuCacheComponent`。这条路**实机跑不到** ——
    //  demo 工程的场景里没有天空盒（探针实证：编辑器一次都没触发解析器），
    //  所以只有这里能证「读得到 cache 组件就真的发 SetEquirect」。
    //
    //  这里手工 emplace cache 组件，不经资源子系统：解析器那一半已经由
    //  Image2D（阶段 2）与 Tilemap（本阶段实机）证过，这里要证的是**消费者**。
    {
        lux::bridgetest::HeadlessBridgeFixture fx;
        lux::ecs::Registry              reg;
        fx.registerSkyboxOps();

        lux::ecs::ActiveRenderView active_view_rs{fx.view()};
        lux::ecs::SceneRenderBinding render_ctx_rs{
            fx.session(), fx.control(), {}, fx.scene()};
        render_ctx_rs.setCatalog(fx.features());
        render_ctx_rs.bindFeature("Skybox", lux::render::FeatureHandle{ 0u, 1u });
        // 特性参数形状 —— 批 B3 起是普通节点;本探针手工驱动。
        lux::ecs::SkyboxSubsystem skybox{};
        skybox.onAdded(lux::ecs::SystemSetupContext{reg, {}});
        fx.beginFrame();

        const auto e = reg.create();
        auto& sc = reg.emplace<lux::ecs::SkyboxComponent>(e);
        sc.equirect_texture_id = lux::asset::asset_id_t::from_string(
            "11111111-2222-3333-4444-555555555555").value();

        drive(skybox, reg, render_ctx_rs, active_view_rs, 1.f / 60.f);
        fx.roundTrip();
        check(fx.recorder().count("SkyboxSetEquirect") == 0,
              "⑤-a：还没有 cache 组件时**不发**（此前是 ensureTexture 返回空句柄）");

        const lux::render::RTextureHandle handle{7u, 1u};
        reg.emplace<lux::ecs::TextureGpuCacheComponent>(
            e, lux::ecs::TextureGpuCacheComponent{handle, sc.equirect_texture_id});

        drive(skybox, reg, render_ctx_rs, active_view_rs, 1.f / 60.f);
        fx.roundTrip();
        check(fx.recorder().count("SkyboxSetEquirect") == 1,
              "⑤-b：cache 组件一出现就发了 SetEquirect（漏了就是天空盒永远不显示）");
        if (fx.recorder().count("SkyboxSetEquirect") == 1)
        {
            const auto p = fx.recorder().payload<lux::render::SkyboxSetEquirectPayload>(
                "SkyboxSetEquirect", 0);
            check(p.texture == handle, "⑤-c：发出去的是 cache 组件里那个句柄");
        }

        drive(skybox, reg, render_ctx_rs, active_view_rs, 1.f / 60.f);
        fx.roundTrip();
        check(fx.recorder().count("SkyboxSetEquirect") == 1,
              "⑤-d：没变化就不重发（PARAM 的脏比较仍然成立）");

        (void)fx.session().beginFrame({});
        fx.submit(); fx.dispatch(); fx.pump();
    }

    // ── ⑥ INSTANCE 子系统的离场（阶段 4c）────────────────────────────────
    //
    //  网格实例的 `reap` 全扫换成了 `on_destroy` 观察者。漏掉的话服务端的实例
    //  永远不删 —— 画面上是「删掉的物体还在」，而 stderr 一字不差。
    //  ⑥-c 专门探 `Exclude`：世界流送靠给实体挂 dormant 标签把它踢出视图，
    //  那是**加**一个组件却要算「离场」，最容易漏连的一条。
    {
        lux::bridgetest::HeadlessBridgeFixture fx;
        lux::ecs::Registry              reg;
        fx.registerMeshStackOps();

        lux::ecs::ActiveRenderView active_view_rs{fx.view()};
        lux::ecs::SceneRenderBinding render_ctx_rs{
            fx.session(), fx.control(), {}, fx.scene()};
        render_ctx_rs.setCatalog(fx.features());
        render_ctx_rs.bindFeature("Skybox", lux::render::FeatureHandle{ 0u, 1u });
        // 批 B3:网格实例是普通节点,本探针手工驱动。
        lux::ecs::MeshSubsystem meshes{};
        meshes.onAdded(lux::ecs::SystemSetupContext{reg, {}});
        fx.beginFrame();

        const auto makeMesh = [&](float x) {
            const auto e = reg.create();
            auto& mc = reg.emplace<lux::ecs::MeshComponent>(e);
            mc.visible = true;
            auto& wt = reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
            wt.linear = Eigen::Matrix3f::Identity();
            wt.position.x = x;
            // 资源子系统的产物 —— 这里手工写，本例要证的是**消费者**的离场。
            reg.emplace<lux::ecs::MeshGpuCacheComponent>(e,
                lux::ecs::MeshGpuCacheComponent{
                    lux::render::RMeshHandle{1u, 1u}, lux::render::RMaterialHandle{1u, 1u}, {}, {}});
            return e;
        };
        const auto frame = [&]
        {
            drive(meshes, reg, render_ctx_rs, active_view_rs, 1.f / 60.f);
            fx.roundTrip();
        };

        const auto e = makeMesh(1.f);
        frame();   // AddMeshInstance 发出
        frame();   // 回复落地 → live
        check(fx.recorder().count("AddMeshInstance") == 1, "⑥-前置：实例建起来了");

        reg.destroy(e);
        check(fx.recorder().count("RemoveMeshInstance") == 0,
              "⑥-a：观察者内**没有**就地发命令（构建器没开，会被静默丢掉）");
        frame();
        check(fx.recorder().count("RemoveMeshInstance") == 1, "⑥-b：RemoveMeshInstance 发出了一次");
        frame();
        check(fx.recorder().count("RemoveMeshInstance") == 1, "⑥-b2：不会重复发");

        const auto transition_entity = makeMesh(2.f);
        reg.emplace<lux::ecs::VisualTransitionComponent>(
            transition_entity,
            lux::ecs::VisualTransitionComponent{0.35f, 0xb9f4f5afu});
        frame();
        frame();
        check(fx.recorder().count("AddMeshInstance") == 2,
              "⑥-d 前置：带视觉过渡的实例建起来了");
        if (fx.recorder().count("AddMeshInstance") == 2)
        {
            const auto add = fx.recorder().payload<
                lux::render::AddMeshInstancePayload>("AddMeshInstance", 1);
            check(add.transition_milliseconds == 350u &&
                      add.transition_seed != 0u,
                  "⑥-d：创建边沿携带组件声明的稳定渐入参数");
        }

        reg.destroy(transition_entity);
        frame();
        check(fx.recorder().count("RetireMeshInstance") == 1,
              "⑥-e：渐出实例离场转交 Render Ghost，而不是留在 Registry");
        check(fx.recorder().count("RemoveMeshInstance") == 1,
              "⑥-f：显式渐出实例不走 transient 的立即 Remove 路径");
        if (fx.recorder().count("RetireMeshInstance") == 1)
        {
            const auto retire = fx.recorder().payload<
                lux::render::RetireMeshInstancePayload>(
                    "RetireMeshInstance", 0);
            check(retire.transition_milliseconds == 350u &&
                      retire.transition_seed != 0u,
                  "⑥-g：渐出复用组件声明的稳定 seed 与时窗");
        }

        (void)fx.session().beginFrame({});
        fx.submit(); fx.dispatch(); fx.pump();
    }

    // ── ⑦ 高亮从「宿主推集合」改成实体标签（阶段 5）──────────────────────
    //
    //  此前编辑器每帧 `RenderSystem::setHighlighted(set)`，上下文存着那份世界状态
    //  的镜像；现在只是实体上的一个 `HighlightedComponent`。漏了的话选中物体不再
    //  发光 —— 一个纯视觉的回归，日志、退出码、测试全都看不见。
    {
        lux::bridgetest::HeadlessBridgeFixture fx;
        lux::ecs::Registry              reg;
        fx.registerMeshStackOps();

        lux::ecs::ActiveRenderView active_view_rs{fx.view()};
        lux::ecs::SceneRenderBinding render_ctx_rs{
            fx.session(), fx.control(), {}, fx.scene()};
        render_ctx_rs.setCatalog(fx.features());
        render_ctx_rs.bindFeature("Skybox", lux::render::FeatureHandle{ 0u, 1u });
        // 批 B3:网格实例是普通节点,本探针手工驱动。
        lux::ecs::MeshSubsystem meshes{};
        meshes.onAdded(lux::ecs::SystemSetupContext{reg, {}});
        fx.beginFrame();

        const auto e = reg.create();
        auto& mc = reg.emplace<lux::ecs::MeshComponent>(e);
        mc.visible = true;
        auto& wt = reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
        wt.linear = Eigen::Matrix3f::Identity();
        reg.emplace<lux::ecs::MeshGpuCacheComponent>(e,
            lux::ecs::MeshGpuCacheComponent{
                lux::render::RMeshHandle{1u, 1u}, lux::render::RMaterialHandle{1u, 1u}, {}, {}});

        const auto frame = [&]
        {
            drive(meshes, reg, render_ctx_rs, active_view_rs, 1.f / 60.f);
            fx.roundTrip();
        };
        frame(); frame();
        check(fx.recorder().count("AddMeshInstance") == 1, "⑦-前置：实例建起来了");
        const auto add = fx.recorder().payload<lux::render::AddMeshInstancePayload>("AddMeshInstance", 0);
        check((add.flags & lux::render::kInstanceFlagHighlight) == 0,
              "⑦-a：没挂标签时创建的实例**不带**高亮位");

        reg.emplace<lux::ecs::HighlightedComponent>(e);
        frame();
        check(fx.recorder().count("UpdateInstanceFlags") == 1,
              "⑦-b：挂上标签 → 推了一次 UpdateInstanceFlags");
        if (fx.recorder().count("UpdateInstanceFlags") >= 1)
        {
            const auto f = fx.recorder().payload<lux::render::UpdateInstanceFlagsPayload>(
                "UpdateInstanceFlags", 0);
            check((f.flags & lux::render::kInstanceFlagHighlight) != 0,
                  "⑦-c：推上去的标志位里**有**高亮位（漏了就是选中不发光）");
        }
        frame();
        check(fx.recorder().count("UpdateInstanceFlags") == 1,
              "⑦-d：标签没变就不重发（last_flags 的脏比较仍然成立）");

        reg.remove<lux::ecs::HighlightedComponent>(e);
        frame();
        check(fx.recorder().count("UpdateInstanceFlags") == 2, "⑦-e：摘掉标签 → 再推一次");
        if (fx.recorder().count("UpdateInstanceFlags") >= 2)
        {
            const auto f = fx.recorder().payload<lux::render::UpdateInstanceFlagsPayload>(
                "UpdateInstanceFlags", 1);
            check((f.flags & lux::render::kInstanceFlagHighlight) == 0, "⑦-f：高亮位清掉了");
        }

        reg.emplace<lux::ecs::AssetStreamingStateComponent>(
            e,
            lux::ecs::AssetStreamingStateComponent{
                7u,
                lux::ecs::EAssetStreamingPhase::GPU_UPLOAD,
                lux::ecs::kDefaultStreamingFeedbackStyle
            }
        );
        frame();
        check(fx.recorder().count("UpdateInstanceFlags") == 3,
              "⑦-g：进入流送状态 → 复用实例 flags 增量通道");
        if (fx.recorder().count("UpdateInstanceFlags") >= 3)
        {
            const auto f = fx.recorder().payload<lux::render::UpdateInstanceFlagsPayload>(
                "UpdateInstanceFlags", 2);
            check((f.flags & lux::render::kInstanceFlagStreamingFeedback) != 0,
                  "⑦-h：流送反馈位已写入 retained instance");
            check((f.flags & lux::render::kInstanceFlagHighlight) == 0,
                  "⑦-i：更新流送位不会污染高亮位");
        }

        reg.remove<lux::ecs::AssetStreamingStateComponent>(e);
        frame();
        check(fx.recorder().count("UpdateInstanceFlags") == 4,
              "⑦-j：流送完成 → 清除反馈位");
        if (fx.recorder().count("UpdateInstanceFlags") >= 4)
        {
            const auto f = fx.recorder().payload<lux::render::UpdateInstanceFlagsPayload>(
                "UpdateInstanceFlags", 3);
            check((f.flags & lux::render::kInstanceFlagStreamingFeedback) == 0,
                  "⑦-k：完成后 retained instance 不再进入 overlay pass");
        }

        (void)fx.session().beginFrame({});
        fx.submit(); fx.dispatch(); fx.pump();
    }

    // ── ⑧ World 的相位表 ─────────────────────────────────────────────────
    //
    //  整个帧的正确性现在压在这上面：`RenderSystem` 只是表里 `kPhaseRender` 那一项，
    //  「谁在渲染之前/之后」全靠相位表达。排错了不报错 —— 表现是「这一帧读到的是
    //  上一帧的值」，日志一字不差。
    {
        std::vector<int> order;

        lux::ecs::World w;
        lux::ecs::Schedule schedule{w};
        // 故意**倒着**注册：相位必须压过注册顺序，否则这一整套就没有意义。
        (void)schedule.addSystem(std::make_unique<PhaseProbe<60>>(&order, 60), lux::ecs::kPhasePostRender);
        (void)schedule.addSystem(std::make_unique<PhaseProbe<30>>(&order, 30), lux::ecs::kPhaseSimulation);
        (void)schedule.addSystem(std::make_unique<PhaseProbe<10>>(&order, 10), lux::ecs::kPhaseInput);
        // 同相位两项：必须保持注册序（域包内部 Transform → Camera → Animation
        // 唯一的排序依据就是它）。
        (void)schedule.addSystem(std::make_unique<PhaseProbe<31>>(&order, 31), lux::ecs::kPhaseSimulation);
        (void)schedule.addSystem(std::make_unique<PhaseProbe<50>>(&order, 50), lux::ecs::kPhaseRender);

        schedule.tick(0.f);
        const std::vector<int> want{10, 30, 31, 50, 60};
        check(order == want, "⑧-a：相位压过注册顺序，同相位保持注册序");

        // through_phase：「补派生数据」不是「跑一帧」——这一条错了就等于每次载入
        // 场景都多发一整帧渲染命令（本次实机对拍抓到的就是它）。
        order.clear();
        schedule.tick(0.f, lux::ecs::kPhaseSimulation);
        const std::vector<int> want2{10, 30, 31};
        check(order == want2, "⑧-b：through_phase 截断在指定相位（渲染相位不跑）");

        // removeSystem：脚本只在 Play 期间存在，靠它摘掉。
        order.clear();
        auto added_probe = schedule.addSystem(
            std::make_unique<PhaseProbe<99>>(&order, 99),
            lux::ecs::kPhaseInput);
        schedule.tick(0.f);
        check(order.size() == 6 && order.front() == 10 && order[1] == 99,
              "⑧-c：新装的进了 Input 相位、排在同相位已有项之后");
        order.clear();
        const bool removed = static_cast<bool>(
            schedule.removeSystem(*added_probe));
        schedule.tick(0.f);
        check(removed && order == want, "⑧-d：removeSystem 摘掉之后顺序不乱");
        check(!schedule.removeSystem(*added_probe),
              "⑧-e：代次失效的 handle 再摘 → 显式失败");
    }

    // ── ⑧′ 系统间的 before/after（拓扑排序，收敛 4）──────────────────────
    //
    //  相位回答不了「我要紧跟在某个具体系统后面」。此前那种约束被编码成
    //  `kPhasePreTransform + 10` 这样的全局坐标，谁再插一个 `+5` 就静默打破它。
    //  类型令牌消除了「名字拼错」；仍需报告缺失的可选系统与声明成环。
    {
        std::vector<int> order;

        {   // 同相位、同注册序都不足以定序 —— 显式约束定了。
            lux::ecs::World w;
            lux::ecs::Schedule schedule{w};
            auto a = std::make_unique<OrderProbe<1>>(&order, 1);
            auto b = std::make_unique<OrderProbe<2>>(&order, 2);
            b->before.push_back(lux::ecs::systemType<OrderProbe<1>>());
            (void)schedule.addSystem(std::move(a), lux::ecs::kPhaseSimulation);
            (void)schedule.addSystem(std::move(b), lux::ecs::kPhaseSimulation);
            const auto rep = schedule.compile();
            order.clear(); schedule.tick(0.f);
            check(order == std::vector<int>{2, 1},
                  "⑧′-a：runsBefore 压过注册序（同相位）");
            check(rep.unknown.empty() && rep.cycle.empty(), "⑧′-b：干净的声明没有诊断");
        }
        {   // **压过相位**：显式声明是作者写下的意图，相位只是默认值。
            //   编辑器相机导航要压过 CameraViewSubsystem::syncAspect，靠的就是这一条。
            lux::ecs::World w;
            lux::ecs::Schedule schedule{w};
            auto early = std::make_unique<OrderProbe<3>>(&order, 1);
            auto late  = std::make_unique<OrderProbe<4>>(&order, 2);
            early->after.push_back(lux::ecs::systemType<OrderProbe<4>>());
            (void)schedule.addSystem(std::move(early), lux::ecs::kPhaseInput);
            (void)schedule.addSystem(std::move(late),  lux::ecs::kPhaseRender);
            const auto rep = schedule.compile();
            order.clear(); schedule.tick(0.f);
            check(order == std::vector<int>{2, 1}, "⑧′-c：显式约束压过相位");
            check(rep.unknown.empty() && rep.cycle.empty(), "⑧′-d：跨相位约束不算异常");
        }
        {   // 可选系统类型缺席 = 那条约束不生效，但必须可诊断。
            lux::ecs::World w;
            lux::ecs::Schedule schedule{w};
            auto a = std::make_unique<OrderProbe<5>>(&order, 1);
            a->after.push_back(lux::ecs::systemType<OrderProbe<99>>());
            (void)schedule.addSystem(std::move(a), lux::ecs::kPhaseSimulation);
            (void)schedule.addSystem(std::make_unique<OrderProbe<6>>(&order, 2), lux::ecs::kPhaseSimulation);
            const auto rep = schedule.compile();
            check(rep.unknown.size() == 1 &&
                      lux::ecs::sameSystemType(
                          rep.unknown[0], lux::ecs::systemType<OrderProbe<99>>()),
                  "⑧′-e：引用未安装的强类型系统 → 报进 unknown");
            order.clear(); schedule.tick(0.f);
            check(order == std::vector<int>{1, 2}, "⑧′-f：无效约束被忽略，退回相位序");
        }
        {   // 成环候选必须在 onAdded / 所有权转移前被拒绝。
            lux::ecs::World w;
            lux::ecs::Schedule schedule{w};
            auto a = std::make_unique<OrderProbe<7>>(&order, 1);
            auto b = std::make_unique<OrderProbe<8>>(&order, 2);
            a->after.push_back(lux::ecs::systemType<OrderProbe<8>>());
            b->after.push_back(lux::ecs::systemType<OrderProbe<7>>());
            const auto a_added = schedule.addSystem(
                std::move(a),
                lux::ecs::kPhaseSimulation
            );
            const auto b_added = schedule.addSystem(
                std::move(b),
                lux::ecs::kPhaseSimulation
            );
            const auto rep = schedule.compile();
            check(a_added && !b_added &&
                      b_added.error() ==
                          lux::ecs::EScheduleMutationError::TopologyCycle &&
                      rep.cycle.empty(),
                  "⑧′-g：成环候选在发布前被拒绝");
            order.clear(); schedule.tick(0.f);
            check(order == std::vector<int>{1},
                  "⑧′-h：被拒候选不污染已发布调度图");
        }
    }

    // ── ⑧′′ 访问集合 → 保守并行批次（暂不改变 tick 的顺序执行）──────────
    {
        lux::ecs::World w;
        lux::ecs::Schedule schedule{w};
        (void)schedule.addSystem(std::make_unique<AccessProbe<1, BatchResourceA, false>>());
        (void)schedule.addSystem(std::make_unique<AccessProbe<2, BatchResourceA, false>>());
        (void)schedule.addSystem(std::make_unique<AccessProbe<3, BatchResourceA, true>>());
        (void)schedule.addSystem(std::make_unique<AccessProbe<4, BatchResourceB, true>>());
        (void)schedule.addSystem(std::make_unique<PhaseProbe<70>>(nullptr, 0));

        const auto rep = schedule.compile();
        const auto batches = schedule.executionBatches();
        check(rep.unknown.empty() && rep.cycle.empty(),
              "⑧′′-a：访问集合不影响拓扑诊断");
        check(batches.size() == 3 &&
                  batches[0].first == 0 && batches[0].count == 2 &&
                  batches[1].first == 2 && batches[1].count == 2 &&
                  batches[2].first == 4 && batches[2].count == 1,
              "⑧′′-b：读读可并行、写冲突分批、未知声明保持独占");
    }

    // ── ⑧″ 系统生命周期（驻留重构批1,设计稿 J5/J7/J9-1）──────────────────
    //
    //  世界是容器与拥有者:addSystem 查重 + 立即 onAdded(注册回调+折入存量),
    //  removeSystem(type) 走 onRemoved,前置缺失在装配末尾响亮报出。每一种
    //  错法的旧表现都是静默:双份系统每帧跑两遍、前置缺失=组件无行为不报错、
    //  晚到系统看不见存量组件(批3 相机域的「主场景全黑」)。
    {
        struct Tag { int v{0}; };   // 折入存量用的组件
        struct Life final : lux::ecs::ISystem
        {
            int  seen_at_add{-1};          // onAdded 时数到的存量 Tag 数
            bool* removed{nullptr};
            void onAdded(const lux::ecs::SystemSetupContext& setup) override
            {
                // 折入存量:晚到的系统在这里看见先于它存在的组件(J9-1)。
                seen_at_add =
                    static_cast<int>(setup.registry().view<Tag>().size());
            }
            void onRemoved(const lux::ecs::SystemRemovalContext&) override
            {
                if (removed) *removed = true;
            }
            [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
            {
                return true;
            }
            void update(const lux::ecs::SystemUpdateContext& /*ctx*/) override {}
        };
        struct MissingCameraSystem final : lux::ecs::ISystem
        {
            void update(const lux::ecs::SystemUpdateContext& /*ctx*/) override {}
        };
        struct NeedyLife final : lux::ecs::ISystem
        {
            std::vector<lux::ecs::SystemType> prereq;
            std::span<const lux::ecs::SystemType> prerequisites() const noexcept override
            {
                return prereq;
            }
            void update(const lux::ecs::SystemUpdateContext& /*ctx*/) override {}
        };

        lux::ecs::World w;
        lux::ecs::Schedule schedule{w};
        const auto e1 = w.createEntity();
        w.emplace<Tag>(e1);
        const auto e2 = w.createEntity();
        w.emplace<Tag>(e2);

        bool removed = false;
        auto sys = std::make_unique<Life>();
        sys->removed = &removed;
        auto* raw = sys.get();
        auto life_added = schedule.addSystem(std::move(sys));
        check(static_cast<bool>(life_added), "⑧″-a：首次注册被接受");
        check(raw->seen_at_add == 2,
              "⑧″-b：onAdded 在收下后立即调、能看见存量组件（折入存量的挂点）");
        check(schedule.hasSystem<Life>() && !schedule.hasSystem<MissingCameraSystem>(),
              "⑧″-c：hasSystem 是在场性的权威答案（组件门控用它）");

        check(!schedule.addSystem(std::make_unique<Life>()),
              "⑧″-d：同类型二次注册被拒（静默双份=每帧跑两遍）");
        {
            const auto rep = schedule.compile();
            check(rep.duplicate.size() == 1 &&
                      lux::ecs::sameSystemType(
                          rep.duplicate[0], lux::ecs::systemType<Life>()),
                  "⑧″-e：被拒的重复类型进报告（装配末尾响亮）");
        }

        auto needy = std::make_unique<NeedyLife>();
        needy->prereq.push_back(
            lux::ecs::systemType<MissingCameraSystem>());
        const auto needy_added = schedule.addSystem(std::move(needy));
        {
            const auto rep = schedule.compile();
            check(!needy_added &&
                      needy_added.error() == lux::ecs::
                          EScheduleMutationError::MissingPrerequisite &&
                      !schedule.hasSystem<NeedyLife>() &&
                      rep.missing_prereq.empty(),
                  "⑧″-f：前置缺失的候选在发布前被拒绝");
            check(rep.duplicate.empty(),
                  "⑧″-g：重名报告是一次性移交,不重复刷");
        }

        auto owned = schedule.removeSystem(*life_added);
        check(owned && removed,
              "⑧″-h：按类型摘除走 onRemoved 并在安全点销毁");
        check(!schedule.hasSystem<Life>() &&
                  !schedule.removeSystem(*life_added),
              "⑧″-i：摘除后不在场;旧 handle 再摘失败");
    }

    // ── ⑨ 引用计数搬去 AssetManager（收敛 3）────────────────────────────
    //
    //  「组件开始用一个资产 → +1，不用了 → −1」的整条链路，端到端。四类错法各一条，
    //  **每一种都不报错**：漏 retain → 资产被流送驱逐而实例还在画（贴图变黑）；
    //  漏 release → GPU 网格永远不销毁（一路涨到进程结束）；归零队列不排空 → 同上；
    //  排空不收敛 → 材质的贴图晚一帧走，拆解路径上就是泄漏（那里没有下一帧）。
    //
    //  日志一个字都不会变，ctest 也照样全过 —— 只有这条能证。
    {
        lux::bridgetest::HeadlessBridgeFixture fx;
        lux::ecs::Registry              reg;
        lux::asset::AssetManager               assets{
            lux::asset::runtimeAssetCodecCatalog()};
        fx.registerMeshStackOps();

        // 最小的一份网格资产（一个三角形）——真的要有，否则 ensureMesh 直接返回空句柄，
        // 解析器在钉引用之前就 return 了，整条路径根本不会跑到。
        lux::asset::asset_id_t mesh_id;
        {
            auto mesh = std::make_unique<lux::rdesc::Mesh>();
            lux::rdesc::Vertex v{};
            v.normal = {0.f, 1.f, 0.f};
            mesh->vertices = {v, v, v};
            mesh->indices  = {0, 1, 2};
            auto a  = assets.createAsset<lux::asset::MeshAsset>(std::move(mesh));
            mesh_id = a->id();
            (void)assets.registerAsset(std::move(a));
        }

        // 驻留三件套(裁决二):声明在 rs 之前 —— 逆序析构,rs 先死。
        ResidencyRig rrig(fx, assets);
        AssetEventRig rig(assets, rrig.residency);   // 批E:广播经总线(声明序=rig 先死,退订先行)
        lux::ecs::ActiveRenderView active_view_rs{fx.view()};
        lux::ecs::SceneRenderBinding render_ctx_rs{
            fx.session(), fx.control(), rrig.async.uploadClient(), fx.scene()};
        render_ctx_rs.setCatalog(fx.features());
        render_ctx_rs.bindFeature("Skybox", lux::render::FeatureHandle{ 0u, 1u });
        // 驻留胶水是普通节点(批 B1)。本探针手工驱动 —— Schedule 走
        // onAdded/update 的那两件事在这里手动做。
            auto residency_glue = rrig.makeGlue(assets);
            residency_glue->resolveMeshOf<lux::ecs::MeshComponent,
                                &lux::ecs::MeshComponent::mesh_asset_id,
                                &lux::ecs::MeshComponent::material_asset_id>();
        residency_glue->attach(reg);
        fx.beginFrame();

        // 生产帧序:帧首泵派发(资产事件)→ 主线程会合 → 世界 tick → 提交往返。
        const auto frame = [&]
        {
            rig.drain();
            rrig.async.drainMainThreadCompletions();
            residency_glue->drainResolvers(reg);
            fx.roundTrip();
            (void)rrig.settleBridge();
        };

        const auto e = reg.create();
        auto& mc = reg.emplace<lux::ecs::MeshComponent>(e);
        mc.visible        = true;
        mc.mesh_asset_id  = mesh_id;   // 材质留 nil：本例证的是引用计数，不是材质
        frame();   // 胶水排空:钉票+await+request → 命中直通上传发出
        check(fx.recorder().count("UploadMesh") == 1, "⑨-前置：网格上传发出了");
        check(assets.isReferenced(mesh_id),
              "⑨-a：请求即钉票(兴趣声明 —— 边沿模型下不先钉,在途行会被归零收割)");
        frame();   // 回执经蹦床在主线程会合点(drainMainThreadCompletions,帧 OPEN)送达 → 组件写上
        check(reg.all_of<lux::ecs::MeshGpuCacheComponent>(e),
              "⑨-b：回执送达,cache 组件写上(等待名单送达,帧 OPEN 相位)");
        check(fx.recorder().count("DestroyMesh") == 0, "⑨-c：还在用，不该销毁");

        reg.destroy(e);
        check(assets.isReferenced(mesh_id),
              "⑨-d：观察者内**不**就地还引用 —— 归零会引出销毁命令，而此刻构建器没开");
        frame();
        check(!assets.isReferenced(mesh_id), "⑨-e：离场排空之后计数归零");
        frame();   // 批E:归零事件在下一帧帧首(泵)派发成销毁命令
        check(fx.recorder().count("DestroyMesh") == 1,
              "⑨-f：归零 → GPU 网格被销毁（归零订阅不接就在这里静默泄漏）");
        frame();
        check(fx.recorder().count("DestroyMesh") == 1, "⑨-g：不会重复销毁");

        // 计数账本自身的边界,现在由票据(AssetRef)语义表达:拷贝/移动/析构与
        // 计数逐一对应。「还一个没 retain 过的 id」在票据下**不可表达** ——
        // 这正是裁决五要的;边界改为「拷贝/移动/未归零的 reset 不得假广播」。
        {
            // 批E:队列探针换事件探针 —— 旁听同一条 AssetUnreferenced 通道。
            std::vector<lux::asset::asset_id_t> zeroed_log;
            auto probe_sub = rig.bus.subscribe<lux::asset::AssetUnreferenced>(
                rig.pump, [&](const lux::asset::AssetUnreferenced& e)
                { zeroed_log.push_back(e.id); });
            {
                auto t1 = assets.acquire(mesh_id);   // +1
                check(assets.isReferenced(mesh_id), "⑨-h：acquire = +1");
                auto t2 = t1;                        // 拷贝 = +1(计数 2)
                auto t3 = std::move(t1);             // 移动不动账
                rig.drain();
                check(zeroed_log.empty(),
                      "⑨-i：拷贝/移动过程中不得出现假归零广播");
                t2.reset();                          // 2→1,不广播
                rig.drain();
                check(zeroed_log.empty() && assets.isReferenced(mesh_id),
                      "⑨-i2：未归零的 reset 不广播");
            }   // t3 析构:1→0,广播
            check(!assets.isReferenced(mesh_id), "⑨-h2：全部票析构后归零");
            rig.drain();   // 同一次派发也送到了缓存:条目早在 ⑨-f 就销毁,
                           // destroyRow 对不认识的 id 是 no-op。
            check(zeroed_log.size() == 1 && zeroed_log[0] == mesh_id,
                  "⑨-i3：归零恰好广播一次");
            probe_sub.reset();
            frame();
            check(fx.recorder().count("DestroyMesh") == 1,
                  "⑨-i4：陈旧归零不重复销毁");
        }

        (void)fx.session().beginFrame({});
        fx.submit(); fx.dispatch(); fx.pump();
        check(fx.recorder().count("DestroyMesh") == 1, "⑨-j：拆解不会再销毁一次已经没了的条目");
    }

    // ── ⑪ 进程域共享缓存:跨世界计数/销毁语义(资产驻留批 3)────────────────
    //
    // 这一批的核心不变量在此,不探则回归完全静默:
    //   a) 两个世界用同一资产 → 只上传一份(此前每 SceneRuntime 一份缓存 = 各传一份);
    //   b) 一个世界松手不销毁另一个世界还在用的副本(全局账本 2→1 不广播);
    //   c) releaseAssetRefs 不再 destroyAll —— 场景拆解只收「因本场景松手而真归零」
    //      的条目,别的世界安全留存;
    //   d) 回执落账后两个世界都拿到同一份 READY 句柄。
    {
        lux::bridgetest::HeadlessBridgeFixture fx;
        lux::ecs::Registry              reg_a, reg_b;   // 两个「世界」
        fx.registerMeshStackOps();

        lux::asset::AssetManager assets{
            lux::asset::runtimeAssetCodecCatalog()};
        lux::asset::asset_id_t   mesh_id{};
        {
            auto mesh = std::make_unique<lux::rdesc::Mesh>();
            lux::rdesc::Vertex v{};
            v.normal = {0.f, 1.f, 0.f};
            mesh->vertices = {v, v, v};
            mesh->indices  = {0, 1, 2};
            auto a  = assets.createAsset<lux::asset::MeshAsset>(std::move(mesh));
            mesh_id = a->id();
            (void)assets.registerAsset(std::move(a));
        }

        // 一套三件套,两个 RenderSystem(声明序 = 逆析构序:三件套最后死)。
        ResidencyRig rrig(fx, assets);
        AssetEventRig rig(assets, rrig.residency);   // 批E:广播经总线
        lux::ecs::ActiveRenderView active_view_rs_a{fx.view()};
        lux::ecs::SceneRenderBinding render_ctx_rs_a{
            fx.session(), fx.control(), rrig.async.uploadClient(), fx.scene()};
        lux::ecs::ActiveRenderView active_view_rs_b{fx.view()};
        lux::ecs::SceneRenderBinding render_ctx_rs_b{
            fx.session(), fx.control(), rrig.async.uploadClient(), fx.scene()};
        // 驻留胶水是普通节点(批 B1)。本探针手工驱动 —— 两个世界各一份胶水,
        // Schedule 走 onAdded/update 的那两件事在这里手动做。
        std::vector<std::unique_ptr<lux::ecs::ResidencySubsystem>> glues;
        for (auto* rctx : {&render_ctx_rs_a, &render_ctx_rs_b})
        {
            rctx->setCatalog(fx.features());
            rctx->bindFeature("Skybox", lux::render::FeatureHandle{ 0u, 1u });
            auto glue = rrig.makeGlue(assets);
            glue->resolveMeshOf<lux::ecs::MeshComponent,
                                &lux::ecs::MeshComponent::mesh_asset_id,
                                &lux::ecs::MeshComponent::material_asset_id>();
            glues.push_back(std::move(glue));
        }
        glues[0]->attach(reg_a);
        glues[1]->attach(reg_b);
        fx.beginFrame();
        const auto frame = [&] {
            rig.drain();   // 帧首派发(生产帧序)
            rrig.async.drainMainThreadCompletions();
            glues[0]->drainResolvers(reg_a);
            glues[1]->drainResolvers(reg_b);
            fx.roundTrip();
            (void)rrig.settleBridge();
        };
        const auto spawn = [&](lux::ecs::Registry& reg) {
            const auto e = reg.create();
            auto& mc = reg.emplace<lux::ecs::MeshComponent>(e);
            mc.visible       = true;
            mc.mesh_asset_id = mesh_id;
            return e;
        };

        const auto ea = spawn(reg_a);
        const auto eb = spawn(reg_b);
        // 上传发出(先到者建条目,后到者看见 pending 早退)。在途窗口在
        // roundTrip 内就闭合(fake server 同步回执),所以断言夹在中间。
        //
        // ★ 批 B1 起 drain 是**显式**的一步:驻留胶水不再藏在 rs.update 里。
        //   此前「rs.update 顺带把驻留也跑了」是一条隐式耦合 —— 扁平化正是要
        //   把这种耦合变成看得见的调用。
        glues[0]->drainResolvers(reg_a);
        glues[1]->drainResolvers(reg_b);
        fx.roundTrip();
        (void)rrig.settleBridge();
        frame();   // 回复已落,本帧 resolver 钉引用 + 写组件
        check(fx.recorder().count("UploadMesh") == 1,
              "⑪-a：两个世界同一资产只上传一份(共享缓存的存在理由)");
        check(reg_a.all_of<lux::ecs::MeshGpuCacheComponent>(ea) &&
              reg_b.all_of<lux::ecs::MeshGpuCacheComponent>(eb),
              "⑪-a2：两边的 cache 组件都写上了(指向同一条目)");

        reg_a.destroy(ea);
        frame();
        check(fx.recorder().count("DestroyMesh") == 0,
              "⑪-b：世界 A 松手不得销毁世界 B 还在用的副本(2→1 不归零)");

        // 世界 A 整体拆解(releaseRefs 路径):同样不得动 B 的副本。
        (void)fx.session().beginFrame({});
        glues[0]->releaseAll();   // 驻留票据不再随 releaseAssetRefs 一起还
        fx.submit(); fx.dispatch(); fx.pump();
        check(fx.recorder().count("DestroyMesh") == 0,
              "⑪-c：A 场景拆解(releaseRefs)不再 destroyAll —— B 的副本留存");

        reg_b.destroy(eb);
        (void)fx.session().beginFrame({});
        glues[1]->drainResolvers(reg_b);
        fx.roundTrip();
        rig.drain();       // 批E:归零事件帧首派发(销毁命令录入当前帧)
        fx.roundTrip();    // 送达服务端
        (void)rrig.settleBridge();
        check(fx.recorder().count("DestroyMesh") == 1,
              "⑪-b2：最后一个世界松手才真正销毁(账本归零边沿)");

        (void)fx.session().beginFrame({});
        glues[1]->releaseAll();
        fx.submit(); fx.dispatch(); fx.pump();
        check(fx.recorder().count("DestroyMesh") == 1, "⑪-c2：拆解不重复销毁");
    }

    // ── ⑬ 内容变更(replaceAsset)→ GPU 副本重建的热更新链(热更新批5)──────
    //
    // bump → content_changed 广播 → onContentChanged 的 destroyRow(force)
    // → 失效推送摘死句柄组件 → 胶水重请求重上传 → 组件句柄变 →
    // MeshInstance 句柄比对拆重建。每一环漏掉都不报错:资产 Save 后场景永远
    // 旧样子(缓存命中旧句柄直接返回),或实例拿着已销毁的句柄继续画。
    // (材质根→实例/贴图→材质的级联收敛在 drainContentChanged 内,fake server
    //  尚无材质 handler,级联专项探针记档待补 —— 本条钉住主链全程。)
    {
        lux::bridgetest::HeadlessBridgeFixture fx;
        lux::ecs::Registry              reg;
        lux::asset::AssetManager               assets{
            lux::asset::runtimeAssetCodecCatalog()};
        fx.registerMeshStackOps();

        lux::asset::asset_id_t mesh_id;
        {
            auto mesh = std::make_unique<lux::rdesc::Mesh>();
            lux::rdesc::Vertex v{};
            v.normal = {0.f, 1.f, 0.f};
            mesh->vertices = {v, v, v};
            mesh->indices  = {0, 1, 2};
            auto a  = assets.createAsset<lux::asset::MeshAsset>(std::move(mesh));
            mesh_id = a->id();
            (void)assets.registerAsset(std::move(a));
        }

        ResidencyRig rrig(fx, assets);
        AssetEventRig rig(assets, rrig.residency);   // 批E:广播经总线
        lux::ecs::ActiveRenderView active_view_rs{fx.view()};
        lux::ecs::SceneRenderBinding render_ctx_rs{
            fx.session(), fx.control(), rrig.async.uploadClient(), fx.scene()};
        render_ctx_rs.setCatalog(fx.features());
        render_ctx_rs.bindFeature("Skybox", lux::render::FeatureHandle{ 0u, 1u });
        // 驻留胶水是普通节点(批 B1)。本探针手工驱动 —— Schedule 走
        // onAdded/update 的那两件事在这里手动做。
            auto residency_glue = rrig.makeGlue(assets);
            residency_glue->resolveMeshOf<lux::ecs::MeshComponent,
                                &lux::ecs::MeshComponent::mesh_asset_id,
                                &lux::ecs::MeshComponent::material_asset_id>();
        residency_glue->attach(reg);
        // 批 B3:网格实例是普通节点,本探针手工驱动。
        lux::ecs::MeshSubsystem meshes{};
        meshes.onAdded(lux::ecs::SystemSetupContext{reg, {}});
        fx.beginFrame();
        const auto frame = [&]
        {
            rig.drain();
            rrig.async.drainMainThreadCompletions();
            residency_glue->drainResolvers(reg);
            drive(
                meshes,
                reg,
                render_ctx_rs,
                active_view_rs,
                1.f / 60.f
            );
            fx.roundTrip();
            (void)rrig.settleBridge();
        };

        const auto e = reg.create();
        auto& mc = reg.emplace<lux::ecs::MeshComponent>(e);
        auto& wt = reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
        wt.linear = Eigen::Matrix3f::Identity();
        mc.visible       = true;
        mc.mesh_asset_id = mesh_id;
        frame(); frame(); frame();   // 上传→组件→实例建成
        check(fx.recorder().count("UploadMesh") == 1 &&
              fx.recorder().count("AddMeshInstance") == 1,
              "⑬-前置：实体 resolved + 实例 live");
        const auto old_handle =
            reg.get<lux::ecs::MeshGpuCacheComponent>(e).mesh;

        // ── 同 id 换内容(文件监视/编辑器 Save 的资产层落点)。──
        {
            auto mesh2 = std::make_unique<lux::rdesc::Mesh>();
            lux::rdesc::Vertex v{};
            v.normal = {1.f, 0.f, 0.f};
            mesh2->vertices = {v, v, v, v, v, v};        // 内容确实不同
            mesh2->indices  = {0, 1, 2, 3, 4, 5};
            auto a2 = assets.createAsset<lux::asset::MeshAsset>(std::move(mesh2));
            a2->mutableInfo()->id = mesh_id;             // 同 id(replace 按 info id 找)
            check(assets.replaceAsset(std::move(a2)), "⑬-前置2：replaceAsset 成功");
        }
        check(assets.contentRevision(mesh_id) == 1, "⑬-a：replace bump 了 revision");
        check(assets.isReferenced(mesh_id), "⑬-b：replace 不动计数(票照常有效)");

        frame();   // 帧首派发 content_changed:destroyRow(force)+失效推送摘组件;
                   // 胶水重请求 → 新行重新上传(命中同步直通,回执本帧
                   // 泵回)—— 帧首处理 + 帧内重请求 = 同一帧闭合(批E 不变)
        check(fx.recorder().count("DestroyMesh") == 1,
              "⑬-c：旧 GPU 副本被强制销毁 —— 票还在也要销(内容已过期)");
        check(fx.recorder().count("UploadMesh") == 2,
              "⑬-d：新内容当帧重新上传(失效推送让胶水重请求)");

        frame();   // 上传回复已落地 → resolver 写新句柄 → 实例句柄比对 → 拆
        check(reg.get<lux::ecs::MeshGpuCacheComponent>(e).mesh != old_handle,
              "⑬-e：cache 组件拿到了新句柄(id 没变,句柄变了)");
        check(fx.recorder().count("RemoveMeshInstance") == 1,
              "⑬-f：实例句柄比对看见「id 同、句柄变」,拆掉 —— 只比 id 会拿死句柄画");

        frame();   // first-sight 重建
        check(fx.recorder().count("AddMeshInstance") == 2,
              "⑬-g：实例带新句柄重建(热更新端到端闭合)");
        check(assets.isReferenced(mesh_id), "⑬-h：全程票不断链");

        reg.destroy(e);
        frame();
        (void)fx.session().beginFrame({});
        fx.submit(); fx.dispatch(); fx.pump();
    }

    // ── ⑭ callback/ticket 不得把 ResidencyAssembly 裸借用带出 owner ───────
    {
        lux::bridgetest::HeadlessBridgeFixture fx;
        lux::asset::AssetManager               assets{
            lux::asset::runtimeAssetCodecCatalog()};
        lux::ecs::ResidencyCallbacks           callbacks;
        lux::runtime::ResidencyAssembly::AssetEventCallbacks
            asset_callbacks;
        lux::ecs::ResidencyCallbacks::Ticket   wait_ticket;
        lux::ecs::ResidencyCallbacks::Ticket   invalidation_ticket;

        int  delivery_hits             = 0;
        int  invalidation_hits         = 0;
        int  failure_hits              = 0;
        bool reentrant_close_observed = false;
        bool inside_failure_callback  = false;
        bool close_completed_inline   = false;
        lux::runtime::ResidencyCloseReport reentrant_close{
            lux::runtime::EResidencyCloseStatus::Closed,
            0
        };
        lux::runtime::ResidencyAssembly* owner = nullptr;

        {
            ResidencyRig rig(
                fx,
                assets,
                [&](const lux::ecs::RenderResourceFailed&)
                {
                    ++failure_hits;
                    inside_failure_callback = true;
                    lux::runtime::detail::subscribeResidencyClose(
                        *owner,
                        [&](lux::runtime::ResidencyCloseReport report)
                        {
                            close_completed_inline =
                                inside_failure_callback;
                            reentrant_close = report;
                            reentrant_close_observed = true;
                        });
                    inside_failure_callback = false;
                }
            );
            owner = &rig.residency;
            callbacks = rig.residency.makeCallbacks();
            asset_callbacks = rig.residency.makeAssetEventCallbacks();

            const auto missing = assets.generateUUID();
            wait_ticket = callbacks.await(
                missing,
                [&](std::uint64_t, const lux::ecs::ResourceFailure*)
                { ++delivery_hits; }
            );
            invalidation_ticket = callbacks.watch_invalidation(
                [&](const std::vector<lux::asset::asset_id_t>&)
                { ++invalidation_hits; }
            );

            // No VFS/provider: ensure terminates asynchronously through the
            // guarded failure sink. A close started in that callback must only
            // enqueue the shared sender-first protocol; it cannot complete on
            // the callback stack or wait for itself.
            callbacks.request(missing, lux::ecs::EResourceDomain::TEXTURE);
            (void)rig.settleBridge();
            check(failure_hits == 1 && reentrant_close_observed
                      && reentrant_close.terminal()
                      && !close_completed_inline,
                  "⑭-a：callback 内 close 异步达到 terminal，不在原栈自等待");

            // Once the close intent is accepted, admission closes immediately
            // at the next MainThreadMailbox safe point. Late requests cannot create a
            // second generation of work while the owner is closing/closed.
            const auto after_reentrant = assets.generateUUID();
            auto accepting_wait = callbacks.await(
                after_reentrant,
                [&](std::uint64_t, const lux::ecs::ResourceFailure*)
                { ++delivery_hits; }
            );
            check(!accepting_wait.active(),
                  "⑭-b：callback close 一经采纳即关闭新订阅 admission");
            accepting_wait.reset();
            callbacks.request(
                after_reentrant,
                lux::ecs::EResourceDomain::TEXTURE
            );
            bool repeated_close_observed = false;
            lux::runtime::detail::subscribeResidencyClose(
                *owner,
                [&](lux::runtime::ResidencyCloseReport report)
                {
                    repeated_close_observed = report.terminal();
                });
            (void)rig.settleBridge();
            check(failure_hits == 1 && repeated_close_observed,
                  "⑭-c：晚请求被拒，重复 close 连接既有终态而不新建流水线");

            const auto terminal = rig.close();
            check(terminal.terminal(),
                  "⑭-d：composition safe point 观察到同一 terminal");

            const int delivery_after_close     = delivery_hits;
            const int invalidation_after_close = invalidation_hits;
            const auto closed_id = assets.generateUUID();
            callbacks.request(
                closed_id,
                lux::ecs::EResourceDomain::TEXTURE
            );
            asset_callbacks.invalidated(closed_id);
            asset_callbacks.content_changed(closed_id);
            asset_callbacks.unreferenced(closed_id);
            asset_callbacks.registered(closed_id);
            auto closed_wait = callbacks.await(
                closed_id,
                [&](std::uint64_t, const lux::ecs::ResourceFailure*)
                { ++delivery_hits; }
            );
            auto closed_invalidation = callbacks.watch_invalidation(
                [&](const std::vector<lux::asset::asset_id_t>&)
                { ++invalidation_hits; }
            );
            check(!closed_wait.active() && !closed_invalidation.active()
                      && delivery_hits == delivery_after_close
                      && invalidation_hits == invalidation_after_close,
                  "⑭-e：terminal 后对象尚存时所有外部 facade 已拒绝新入口");
        }

        const int delivery_before_late     = delivery_hits;
        const int invalidation_before_late = invalidation_hits;
        const auto late_id = assets.generateUUID();

        // Every callable and opaque ticket deliberately outlives the assembly.
        // Old callbacks become no-ops; new registration returns an empty owner;
        // releasing old tickets must not call the destroyed service.
        callbacks.request(late_id, lux::ecs::EResourceDomain::TEXTURE);
        asset_callbacks.invalidated(late_id);
        asset_callbacks.content_changed(late_id);
        asset_callbacks.unreferenced(late_id);
        asset_callbacks.registered(late_id);
        auto late_wait = callbacks.await(
            late_id,
            [&](std::uint64_t, const lux::ecs::ResourceFailure*)
            { ++delivery_hits; }
        );
        auto late_invalidation = callbacks.watch_invalidation(
            [&](const std::vector<lux::asset::asset_id_t>&)
            { ++invalidation_hits; }
        );
        wait_ticket.reset();
        invalidation_ticket.reset();

        check(!late_wait.active() && !late_invalidation.active(),
              "⑭-f：owner 析构后的 callback 注册返回空 ticket");
        check(delivery_hits == delivery_before_late
                  && invalidation_hits == invalidation_before_late,
              "⑭-g：晚 request/reset 不触达已销毁 assembly/service");
    }

    // ── ⑩ attach 解析器（根 + 闭包 + 稳定拓扑序）纯函数探针 ─────────────────
    //
    // 为什么非探不可:解析序 ≠ 旧目录序的错法**不报错、只画错**(同 stage
    // 写后写的叠放、attach 期缓存的裸指针都对顺序敏感)。⑩-f 断言的「根集合
    // 不变 ⇒ 输出 == 声明序过滤结果」正是设计的向后兼容承诺。
    {
        using namespace lux::render;
        // 目录声明序:A、B(需 A)、D(需 B + 可选 C)、C、F(需未注册的 G)、
        // H(需 I)、I(需 H)。C 刻意声明在 D **之后**,让 ⑩-b 只能靠可选边赢。
        static constexpr FeatureDependency kBDeps[]{{featureId("probe.a"), false}};
        static constexpr FeatureDependency kDDeps[]{{featureId("probe.b"), false},
                                                    {featureId("probe.c"), true}};
        static constexpr FeatureDependency kFDeps[]{{featureId("probe.g"), false}};
        static constexpr FeatureDependency kHDeps[]{{featureId("probe.i"), false}};
        static constexpr FeatureDependency kIDeps[]{{featureId("probe.h"), false}};
        const auto mk = [](const char* name, FeatureTypeId type,
                           std::span<const FeatureDependency> deps)
        {
            FeatureFactory f{};
            f.name                    = name;
            f.descriptor.type         = type;
            f.descriptor.dependencies = deps;
            return f;
        };
        FeatureCatalog cat;
        cat.add(mk("A", featureId("probe.a"), {}),     1, {});
        cat.add(mk("B", featureId("probe.b"), kBDeps), 2, {});
        cat.add(mk("D", featureId("probe.d"), kDDeps), 3, {});
        cat.add(mk("C", featureId("probe.c"), {}),     4, {});
        cat.add(mk("F", featureId("probe.f"), kFDeps), 5, {});
        cat.add(mk("H", featureId("probe.h"), kHDeps), 6, {});
        cat.add(mk("I", featureId("probe.i"), kIDeps), 7, {});

        {
            const std::string_view roots[]{ "D" };
            const auto r = cat.resolveAttachOrder(roots);
            check(r.order.size() == 3 && r.order[0] == "A" &&
                  r.order[1] == "B" && r.order[2] == "D",
                  "⑩-a:必需依赖闭包拉入 + 拓扑序 A→B→D");
            check(r.unknown.empty() && r.missing_deps.empty() && r.cycle.empty(),
                  "⑩-a:干净解析零记账");
        }
        {
            const std::string_view roots[]{ "D", "C" };
            const auto r = cat.resolveAttachOrder(roots);
            std::size_t ci = 99, di = 99;
            for (std::size_t i = 0; i < r.order.size(); ++i)
            {
                if (r.order[i] == "C") ci = i;
                if (r.order[i] == "D") di = i;
            }
            check(r.order.size() == 4 && ci < di,
                  "⑩-b:可选依赖不拉闭包,但在场时作排序边(C 先于 D,虽声明更晚)");
        }
        {
            const std::string_view roots[]{ "Zed" };
            const auto r = cat.resolveAttachOrder(roots);
            check(r.order.empty() && r.unknown.size() == 1 && r.unknown[0] == "Zed",
                  "⑩-c:根不在目录 → unknown 记账(旧「声明了却没挂上」warning 的账本)");
        }
        {
            const std::string_view roots[]{ "F" };
            const auto r = cat.resolveAttachOrder(roots);
            check(r.missing_deps.size() == 1 && r.missing_deps[0].dependent == "F" &&
                  r.order.size() == 1 && r.order[0] == "F",
                  "⑩-d:缺注册的必需依赖记账;被依赖方留在 order(服务端闸响亮拒绝,不静默跳过)");
        }
        {
            const std::string_view roots[]{ "H" };
            const auto r = cat.resolveAttachOrder(roots);
            check(r.cycle.size() == 2 && r.order.size() == 2 &&
                  r.order[0] == "H" && r.order[1] == "I",
                  "⑩-e:依赖环记账 + 回退声明序附尾");
        }
        {
            const std::string_view roots[]{ "C", "A" };
            const auto r = cat.resolveAttachOrder(roots);
            check(r.order.size() == 2 && r.order[0] == "A" && r.order[1] == "C",
                  "⑩-f:并列决胜 = 目录声明序(根集合不变 ⇒ 输出即声明序过滤 —— 叠放语义的保证)");
        }
    }

    std::fprintf(stderr, "\n%s (%d failure(s))\n", g_failures ? "PROBE FAILED" : "PROBE PASSED", g_failures);
    return g_failures ? 1 : 0;
}
