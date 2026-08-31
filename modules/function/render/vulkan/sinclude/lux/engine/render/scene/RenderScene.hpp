#pragma once
/**
 * @file RenderScene.hpp
 * @brief RenderScene — a render-module scene container.
 *
 * Owns per-scene state: features, views, render-graph infrastructure,
 * and per-scene material pipeline.  Receives a shared RenderContext
 * (GPU resources, pipelines, uploads) via constructor — RAII, no init().
 *
 * Usage:
 *   auto ctx = RenderContext::create(res_ctx, std::move(ci));
 *   if (!ctx) return lux::cxx::unexpected(ctx.error());
 *   RenderScene scene(*ctx, {.scene_name = "Main"});
 *   scene.addFeature<ForwardMeshFeature>(config);
 *   // ... per-frame: beginFrame → updateView →
 *   //                renderer.render(scene, cmd, fi) → endFrame
 *
 * Thread ownership: all methods are **render-thread only** unless documented
 * otherwise (`sceneControlMailbox()` / `sceneEvents()` / `syncProducer()` are game-thread safe).
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/core/FrameStamp.hpp>
#include <lux/engine/function/render/client/core/RenderEntityId.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>

#include <iosfwd> // std::ostream (dumpCompiledGraph)
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/FrameRetireScheduler.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp> // sceneId()
#include <lux/engine/function/render/client/core/RenderTypes.hpp>   // lux::math::Extent2u
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/scene/SceneGraphCache.hpp>  // SceneGraphState + 图缓存组件
#include <lux/engine/render/scene/SceneViewSet.hpp>     // ViewCreateInfo + 视图集合组件
#include <lux/engine/render/scene/RenderFeatureSet.hpp> // 特性容器 + 查询 + 每(特性,视图)账本
#include <lux/engine/render/scene/PipelineConfig.hpp>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/render/gpu/lifecycle/ResourceRegistry.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/gpu/transfer/TransferScheduler.hpp>
#include <lux/engine/math/AABB.hpp>
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp> // SlotKeyAutoSparseSet (generational views)

#include <entt/entity/registry.hpp>

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <type_traits>
#include <unordered_set>
#include <vector>

// Forward declarations — avoid pulling in heavy headers
struct VkCommandBuffer_T; // Vulkan opaque handle
using VkCommandBuffer = VkCommandBuffer_T*;

namespace lux::render
{
    class RenderContext;
    class RGVulkanRecorder;
    class RGVulkanResourceAllocator;
    struct RGCompiledGraph;
    struct RGGraphDescription;
    struct RGRecordContext;
    struct RGResourceState;
    class SceneResources;
    class SceneDomainDescriptorSets;
    class FeatureTypeRegistry;

    // (namespace extract 的 IEntitySink / ITransformSink / ILightSink 前向声明已删除
    //  —— 全仓只有这三行声明,没有任何定义。真实的摄取通道是 comm 协议。
    //  见分层设计 §12.7。)

    // (SceneGraphState 已迁入 scene/SceneGraphCache.hpp —— 编译图连同它的
    //  基础设施与退休队列一起,由 SceneGraphCache 持有。本头 include 之,故
    //  Renderer 等既有消费者的 scene.graphState() 用法完全不变。)

    // (ViewCreateInfo 已迁入 scene/SceneViewSet.hpp —— 它是建视图的入参,随视图
    //  集合一起走。本头 include 之,故 Config::initial_views 与 ui 侧用法不变。)

    // ─────────────────────────────────────────────────────────────────────
    //  RenderScene
    // ─────────────────────────────────────────────────────────────────────

    struct MeshBinding final
    {
        RenderObjectHandle object{};
    };

    struct LightBinding final
    {
        RLightHandle light{};
    };

    class LUX_FUNCTION_PUBLIC RenderScene
    {
    public:
        using EntityRegistry = entt::basic_registry<RenderEntityId>;

        struct Config
        {
            std::string scene_name{"RenderScene"};
            PipelineConfig pipeline{};
            std::vector<ViewCreateInfo> initial_views{};
            double coordinate_page_size{1024.0};
            std::int64_t scene_origin_page[3]{};
        };

        /// Construct from a fully-initialized RenderContext (RAII).
        /// Creates per-scene render-graph infrastructure internally.
        explicit RenderScene(std::shared_ptr<RenderContext> ctx);
        RenderScene(std::shared_ptr<RenderContext> ctx, const Config& cfg);
        ~RenderScene();

        RenderScene(const RenderScene&) = delete;
        RenderScene& operator=(const RenderScene&) = delete;
        RenderScene(RenderScene&&) = delete;
        RenderScene& operator=(RenderScene&&) = delete;

        // ================================================================
        //  Shutdown
        // ================================================================

        /// Explicit cleanup (features, RG infra).  Automatically called by
        /// the destructor; safe to call multiple times.
        void shutdownFull();

        // ================================================================
        //  Feature Management
        // ================================================================

        /**
         * @brief Add a feature and return its assigned feature_id.
         *
         * The caller must save the returned id for later access via
         * getFeature() / getFeatureAs<T>().
         */
        template <typename T, typename... Args> Expected<FeatureHandle> addFeature(Args&&... args)
        {
            return installFeature(std::make_unique<T>(std::forward<Args>(args)...));
        }

        /// Type-erased feature install for the public SDK plugin path
        /// (lux::render::addFeature, used by a FeatureFactory create_fn).
        /// In-module callers should prefer the templated addFeature<T>() above;
        /// this exists so an external plugin's create_fn can install a feature
        /// without ever seeing the full RenderScene definition.
        Expected<FeatureHandle> addFeatureErased(std::unique_ptr<RenderFeature> feature);

        /**
         * @brief 装载一个特性(事务性)。addFeature<T> 与插件路径都走这里。
         *
         * 三段式,因为**只有中间那段需要具体类型**:
         *   1. beginInstall  依赖/冲突/多重性/特性等级校验 + attach。类型擦除。
         *   2. 插入容器      RenderFeatureSet::insert<T> 在这里用 if constexpr 判定
         *                    "产不产 pass" —— 类型信息到此为止,不再外泄。
         *   3. finishInstall 置启用态、回填已有视图的每视图状态(失败则整体回滚)。
         *                    类型擦除。
         *
         * 拆成三段而不是把一个 RenderFeature* 标志参数塞进单一函数 —— 后者要调用
         * 方替容器算它自己的事,签名也说不清那个参数和第一个参数是同一个对象。
         */
        template <typename T> Expected<FeatureHandle> installFeature(std::unique_ptr<T> feature)
        {
            static_assert(std::is_base_of_v<RenderFeature, T>, "只能装入 RenderFeature 及其派生类");
            if (auto begun = beginInstall(*feature); !begun)
                return lux::cxx::unexpected(begun.error());

            auto* raw = feature.get();
            const FeatureHandle handle = feature_set_.insert(std::move(feature));
            return finishInstall(*raw, handle);
        }

        /// O(1) lookup by handle.  Returns nullptr if the handle is invalid or stale.
        [[nodiscard]] RenderFeature* getFeature(FeatureHandle feature_id) const;

        /// Typed convenience — static_cast after O(1) lookup.
        template <typename T> T* getFeatureAs(FeatureHandle feature_id) const
        {
            return static_cast<T*>(getFeature(feature_id));
        }

        Expected<void> removeFeature(FeatureHandle feature_id);

        /// Attach the feature's type-level descriptor (from its FeatureFactory) to
        /// the instance. Called by the server right after the factory creates it; the
        /// descriptor then drives type identity, dependency/conflict checks, and the
        /// lifecycle state machine's capability gating. No-op if the handle is stale.
        void setFeatureDescriptor(FeatureHandle feature_id, const FeatureDescriptor& descriptor) noexcept;

        /// RAII bracket around a factory create_fn call: makes the feature it
        /// constructs receive its type-level descriptor BEFORE initAndAttachTo, so the
        /// feature can observe its own descriptor (deps / conflicts / capability flags)
        /// during attach instead of only after. The comm install paths wrap create_fn
        /// in one of these; addFeatureImpl applies the pending descriptor pre-attach.
        /// Replaces the old "create then setFeatureDescriptor afterwards" two-step,
        /// which left the descriptor invisible during attach.
        class FeatureInstallScope
        {
        public:
            FeatureInstallScope(RenderScene& scene, const FeatureDescriptor& desc) noexcept : scene_(scene)
            {
                scene_.pending_install_descriptor_ = &desc;
            }
            ~FeatureInstallScope()
            {
                scene_.pending_install_descriptor_ = nullptr;
            }
            FeatureInstallScope(const FeatureInstallScope&) = delete;
            FeatureInstallScope& operator=(const FeatureInstallScope&) = delete;

        private:
            RenderScene& scene_;
        };

        /// True if any live feature in this scene has the given stable type id.
        /// (kInvalidFeatureTypeId never matches — untyped features are excluded.)
        [[nodiscard]] bool hasFeatureOfType(FeatureTypeId type) const noexcept;

        /// Renderer-owned bookkeeping seam. Bound exactly once immediately
        /// after construction, before a registered feature can be installed.
        void bindFeatureTypeRegistry(FeatureTypeRegistry& registry) noexcept
        {
            feature_type_registry_ = &registry;
        }

        /// Remove all features from this scene.  Invalidates the render graph.
        void removeAllFeatures();

        Expected<void> setFeatureEnabled(FeatureHandle feature_id, bool enabled);
        Expected<void> setFeatureEnabled(std::string_view feature_name, bool enabled);

        // (FeatureInfo + queryFeatures() 已删除:全仓零调用点。)

        /// Per-feature param descriptor for the editor's feature-settings panel.
        /// `struct_name`/`data`/`size` are empty/null/0 for features that expose
        /// no tunable params (paramStructName() == ""). All views point at
        /// render-thread-owned storage valid for the duration of the call.
        /// 编辑器特性设置面板用;结构随实现迁入 RenderFeatureSet。
        /// 保留别名:comm 的 QueryFeatureParams handler 显式命名了
        /// RenderScene::FeatureParamDesc(lambda 形参),别名让它零改动。
        using FeatureParamDesc = RenderFeatureSet::FeatureParamDesc;
        std::vector<FeatureParamDesc> queryFeatureParamDescs() const;

        /// Dense vector of all live features (for iteration).
        /// 全部已装特性(含未启用)。
        [[nodiscard]] std::span<RenderFeature* const> features() const noexcept;
        /// 已启用的特性。
        [[nodiscard]] std::span<RenderFeature* const> enabledFeatures() const noexcept;

        /// 全部已装特性的额外输出槽需求并集(TargetSlot 位掩码)。
        /// 按**已装**而非已启用计 —— 关掉特性不缩池(影像小,进出反而抖动)。
        /// 免缓存:调用方是编排器的每(目标,场景)对逐帧检查,N≈特性数,
        /// 纯虚调用无字符串无哈希,不值得为它上脏标记。
        [[nodiscard]] uint32_t requiredTargetSlotMask() const noexcept;

        // (ERenderPath / renderPath() / setRenderPath 整组已删,连同 RenderPath.hpp。
        //  render_path_ 没有任何读者,setRenderPath 没有任何调用者,而 setRenderPath
        //  的注释("removes all features, re-adds features appropriate for the path")
        //  描述的是一件它从未做过的事。渲染路径的真实表达是**装哪些 feature**
        //  ——orchestrator 按 scene kind 选 ForwardMesh 或 DeferredGBuffer+Lighting
        //  ——而不是翻一个没人读的枚举。)

        /// Mark the render graph as invalid so it will be recompiled next frame.
        void invalidateGraph(EGraphInvalidationReason reason = EGraphInvalidationReason::UNKNOWN) noexcept;

        /// Debug: write a human-readable dump of the CURRENT compiled render
        /// graph (execution order + per-pass reads/writes/barriers) to @p os.
        /// No-op note printed if the graph isn't compiled yet. Render-thread.
        void dumpCompiledGraph(std::ostream& os) const;

        /// Write the latest fence-retired per-view GPU timing snapshots as JSON.
        /// The data was adopted without VK_QUERY_RESULT_WAIT_BIT and is immutable
        /// for the duration of this render-thread call.
        void dumpGpuTiming(std::ostream& os) const;

        /// Suppress automatic graph recompilation during batch feature changes.
        /// While suppressed, `prepareSceneForRender` will skip recompile even if
        /// the graph is invalid.  Call `resumeGraphRecompile()` when done.
        void suppressGraphRecompile() noexcept
        {
            recompile_suppressed_ = true;
        }

        /// Resume automatic graph recompilation.
        void resumeGraphRecompile() noexcept
        {
            recompile_suppressed_ = false;
        }

        /// Whether graph recompilation is currently suppressed.
        [[nodiscard]] bool isGraphRecompileSuppressed() const noexcept
        {
            return recompile_suppressed_;
        }

        // ================================================================
        //  Pipeline Configuration
        // ================================================================

        [[nodiscard]] const PipelineConfig& pipelineConfig() const noexcept
        {
            return pipeline_config_;
        }
        [[nodiscard]] double spatialTileSize() const noexcept
        {
            return config_.coordinate_page_size;
        }
        [[nodiscard]] const std::int64_t* sceneOriginPage() const noexcept
        {
            return config_.scene_origin_page;
        }
        [[nodiscard]] Expected<void> rebaseSceneOrigin(const std::int64_t scene_origin_page[3]) noexcept;

        /// Replace the pipeline configuration.  Invalidates the render graph.
        void setPipelineConfig(PipelineConfig cfg) noexcept
        {
            pipeline_config_ = cfg;
            invalidateGraph(EGraphInvalidationReason::FEATURE_TOPOLOGY);
        }

        /// Set scene-global time (called from comm handler).
        void setSceneTime(float total, float delta, uint64_t frame_number) noexcept
        {
            scene_time_ = total;
            scene_delta_time_ = delta;
            scene_total_frames_ = frame_number;
        }
        [[nodiscard]] float sceneTime() const noexcept
        {
            return scene_time_;
        }
        [[nodiscard]] std::uint64_t sceneFrameNumber() const noexcept
        {
            return scene_total_frames_;
        }

        // ================================================================
        //  Scene Service Registry
        // ================================================================

        // ================================================================
        //  View Management
        // ================================================================

        [[nodiscard]] ViewHandle addView(const ViewCreateInfo& info);
        /// 返回 false = 幂等守卫拒绝(句柄陈旧 / 已在销毁中),什么都没做。
        /// comm handler 据此回 GenericOkReply 的失败码;服务端内部调用方
        /// (UIRenderServer 的 swapchain 链)对重复摘不在乎,(void) 掉即可。
        bool removeView(ViewHandle handle);
        [[nodiscard]] View* getView(ViewHandle handle) noexcept;
        [[nodiscard]] const View* getView(ViewHandle handle) const noexcept;

        void forEachActiveView(auto&& fn)
        {
            for (auto* view : view_set_.active())
                fn(*view);
        }

        void forEachActiveView(auto&& fn) const
        {
            for (auto* view : view_set_.active())
                fn(*view);
        }

        void endViewFrame(uint32_t frame_slot);

        // ── Domain-neutral per-frame primitives (general; no domain knowledge) ──
        // The monotonic frame serial of the frame currently being recorded.
        [[nodiscard]] uint64_t frameSerial() const noexcept
        {
            return current_stamp_.serial;
        }

        /// Per-frame "instance cull-enable mask" GPU address (buffer-device-address;
        /// 0 = none). A general primitive the GPU-driven mesh cull reads to skip
        /// disabled instances — the core does NOT know who produces it. A feature
        /// (e.g. SpatialCullFeature) sets it each frame in onFrameBegin; it is reset
        /// to 0 at the start of every beginFrame, so a scene with no provider gets 0
        /// (= every instance active). This is the decoupling seam that keeps the core
        /// free of SpatialCullGrid / large-world concepts.
        void setInstanceCullMaskAddress(uint64_t addr) noexcept
        {
            instance_cull_mask_addr_ = addr;
        }
        [[nodiscard]] uint64_t instanceCullMaskAddress() const noexcept
        {
            return instance_cull_mask_addr_;
        }

        // ── View lifecycle (all View state is driven by Scene) ──────────

        // (initViewUBO / destroyViewUBO / endViewFrame(View&) 已迁入 SceneViewSet
        //  —— 每视图 GPU 槽的分配与释放是视图生命周期的一部分,且全仓无模块外调用方。
        //  resizeView 早已消亡:改尺寸走 ResizeTarget(View 瘦身)。)

        // ================================================================
        //  Per-Frame Lifecycle (render thread)
        // ================================================================

        void beginFrame(const FrameStamp& stamp);

        /// Record scene-local uploads for this tick (instance/point-cloud/trajectory/scene buffers).
        void recordUploads(VkCommandBuffer cmd, const FrameStamp& stamp);

        /// Convenience entry — forwards to recordUploads().
        void record(VkCommandBuffer cmd, const FrameStamp& stamp)
        {
            recordUploads(cmd, stamp);
        }

        // (updateView removed — the View type no longer bakes in 3D-specific
        // concepts: per-view camera data is now a feature domain.
        // StandardViewCamera fills ViewCameraResource + the View's neutral GPU
        // staging directly; the core scene no longer names camera matrices /
        // frustum and no longer broadcasts onFrustumUpdated.)

        void endFrame(uint64_t frame_id, uint64_t completed_serial);
        // ================================================================
        //  Render-Graph / Pass Infrastructure
        // ================================================================
        RGVulkanResourceAllocator& graphAllocator() noexcept;
        RGVulkanRecorder& graphRecorder() noexcept;

        /// Access the (possibly invalid) scene-level compiled graph.
        [[nodiscard]] SceneGraphState& graphState() noexcept;
        [[nodiscard]] const SceneGraphState& graphState() const noexcept;

        /**
         * @brief Compile (or recompile) the scene-level render graph from a target layout.
         *
         * Collects addPasses() from all enabled features, produces an
         * RGCompiledGraph, and updates graph_state_.
         * Called by Renderer or by external code when the graph is
         * invalidated (feature change, resize, etc.).
         *
         * The layout describes which slots (SceneColor, SceneDepth, …) the target
         * provides and their format/usage properties.  The slot descriptors are baked
         * into the compiled graph; actual per-frame VkImage handles are supplied
         * through RenderRequest::target at record time.
         */
        void compileGraphTemplate(const RenderTargetLayout& layout);

        /// 调试名(诊断打印用;格式异构报错点名)。
        [[nodiscard]] const std::string& debugName() const noexcept
        {
            return debug_name_;
        }

        /// 本场景在 Renderer 里的生成式 id。Renderer::addScene 在插入后回填 ——
        /// 场景此前不知道自己的 id,于是它自发上报的诊断没法说清是**哪个**场景的
        /// (诊断面板上「有个场景没有 SceneColor」等于没说)。
        [[nodiscard]] RenderSceneId sceneId() const noexcept
        {
            return scene_id_;
        }
        void setSceneId(RenderSceneId id) noexcept
        {
            scene_id_ = id;
            graph_cache_->setSceneIndex(id.index);
        }

        /// Invalidate all view resource states.  Called after graph recompilation
        /// so each view re-allocates resources on next render.
        // (invalidateAllViewResources 已删除:全仓零外部调用,它唯一的用途是编译提交点
        //  收走视图资源,现由 SceneGraphCache::compile 内部完成。)

        /// Defer destruction of a previous per-view RG resource state.
        /// Used when resizing or swapping view resources while old GPU work may still be in flight.
        void retireViewResourceState(RGResourceState&& state, const RGGraphDescription* source_graph);

        // ================================================================
        //  Scene Data
        // ================================================================
        RenderContext& renderContext() noexcept;
        const RenderContext& renderContext() const noexcept;

        /// SoA slot index for this scene's entry in SceneResources::SceneGlobalBuffer.
        [[nodiscard]] SlotHandle sceneGlobalSlot() const noexcept
        {
            return scene_global_slot_;
        }
        [[nodiscard]] FrameRetireScheduler::OwnerToken retireOwnerToken() const noexcept
        {
            return retire_owner_token_;
        }

        // ================================================================
        //  Primitive Registry
        // ================================================================

        [[nodiscard]] EntityRegistry& entities() noexcept
        {
            return entities_;
        }
        [[nodiscard]] const EntityRegistry& entities() const noexcept
        {
            return entities_;
        }

        // ================================================================
        //  Scene Resource Registry
        // ================================================================

        [[nodiscard]] ResourceRegistry& resources() noexcept
        {
            return resources_;
        }
        [[nodiscard]] const ResourceRegistry& resources() const noexcept
        {
            return resources_;
        }

        /// Per-scene growable descriptor-pool chain backing this scene's
        /// persistent descriptor sets (light/scene/instance/vertex-pool/shadow/
        /// skinning). Layouts stay global; only set allocation is per-scene.
        [[nodiscard]] SceneDescriptorArena& descriptorArena() noexcept
        {
            return scene_descriptor_arena_;
        }

        /// The descriptor set merged by bind-frequency domain. During the
        /// transition it coexists alongside the older per-set batch.
        [[nodiscard]] SceneDomainDescriptorSets* domainDescriptorSets() noexcept
        {
            return scene_domain_sets_.get();
        }

        /// Per-scene transfer scheduler — a feature registers its per-scene SSBO as a
        /// contributor (makeTransferContributor) so it flushes before draw. Used by
        /// LightFeature for its LightResources (the scene ctor no longer knows light).
        [[nodiscard]] TransferScheduler& transferScheduler() noexcept
        {
            return transfer_scheduler_;
        }

    private:
        /// 只看 FeatureDescriptor 声明的关系:多重性、冲突、必需依赖、特性等级档案。
        /// 四道闸全在 attach 之前,拒绝时没有任何东西要回滚。
        [[nodiscard]] Expected<void> validateDeclaredRelationships(const FeatureDescriptor& desc) const;

        /// 装载事务的第 1 段:校验 + attach。失败即拒绝装载(此时尚未插入容器、
        /// 也没登记任何东西,无需回滚);错误原样来自被拒的那道闸或特性自己的 attach。
        [[nodiscard]] Expected<void> beginInstall(RenderFeature& feature);
        /// 装载事务的第 3 段:置启用态 + 回填每视图状态(失败则整体回滚)。
        [[nodiscard]] Expected<FeatureHandle> finishInstall(RenderFeature& feature, FeatureHandle handle);

        // Core feature removal. check_reverse_deps=true (public removeFeature) refuses
        // to remove a feature another installed feature still requires; =false
        // (removeAllFeatures / shutdownFull) bypasses the guard for bulk teardown.
        Expected<void> removeFeatureInternal(FeatureHandle feature_id, bool check_reverse_deps);

        // Set for the duration of a factory create_fn call (see FeatureInstallScope):
        // addFeatureImpl applies it to the feature BEFORE initAndAttachTo so the
        // descriptor is visible during attach. Null outside an install.
        const FeatureDescriptor* pending_install_descriptor_{nullptr};
        FeatureTypeRegistry* feature_type_registry_{nullptr};

        /// Renderer::addScene 插入后回填(见 sceneId())。
        RenderSceneId scene_id_{};

        // ── Per-(feature, view) state ownership (truth source) ───────────────
        // Records which (feature, view) pairs have had allocateViewState()
        // succeed, so deallocateViewState() runs exactly once — symmetrically on
        // view removal AND feature removal — instead of the old enabled-only,
        // never-on-feature-removal scheme that leaked. Keyed by feature_id; the
        // inner set holds the view ids that feature currently owns state for.
        // (Lifecycle events are rare and feature/view counts small, so a plain
        // map-of-set is fine; not a per-frame hot path.) See ensure/release below.

        /// Allocate this feature's state for @p view_index if not already present.
        /// Idempotent. Returns false if allocateViewState() failed (nothing recorded).
        /// Keyed by the view's index (== ViewHandle::index); the generation is a
        /// scene-level concern validated at the views_ lookup boundary, so the
        /// feature-facing per-view id stays a bare uint32.
        bool ensureFeatureViewState(RenderFeature& feature, uint32_t view_index);

        /// Release this feature's state for @p view_index if present. Idempotent;
        /// calls deallocateViewState() exactly once for a recorded pair.
        void releaseFeatureViewState(RenderFeature& feature, uint32_t view_index) noexcept;

        // (三条延迟销毁 FIFO 全部迁出:两条图相关的在 SceneGraphCache,
        //  视图那条 pending_destroys_ 在 SceneViewSet。)

        std::shared_ptr<RenderContext> render_ctx_;
        Config config_;
        PipelineConfig pipeline_config_;

        // ── Per-scene descriptor-pool chain (growable; backs all per-scene
        //    persistent descriptor sets). Declared BEFORE resources_ so it
        //    is destroyed AFTER it during member destruction (reverse order). ──
        SceneDescriptorArena scene_descriptor_arena_;
        /// The descriptor set merged by bind-frequency domain.
        /// Held via unique_ptr rather than as a value member: its definition
        /// pulls in the whole EngineSetShapes -> LayoutContract chain, and
        /// RenderScene.hpp is a heavily-included header across ecs/editor —
        /// including it directly would pollute downstream TUs' include
        /// surface and break the build (confirmed empirically).
        std::unique_ptr<SceneDomainDescriptorSets> scene_domain_sets_;

        // ── Per-scene resource registry ──────────────────────────────────────
        ResourceRegistry resources_;

        // Stable cross-lane entities own only bindings to specialized Render
        // resources; resource payloads remain in their dedicated stores.
        EntityRegistry entities_;

        /// 特性容器、查询与每(特性,视图)状态账本(纯数据;事务编排留在本类)。
        RenderFeatureSet feature_set_;

        /// 编译图 + 图基础设施 + 图相关退休。unique_ptr 而非值成员:它的构造需要
        /// debug_name_,而后者按声明序在本成员之后初始化 —— 值成员会用到未初始化的
        /// 名字。构造体内建立(正是旧 allocator_/recorder_ 的建立处)。
        ///
        /// ⚠️ **必须声明在 view_set_ 之前**:视图持有的图资源(录制上下文 + 物理资源)
        /// 归它所有,`~SceneViewSet()` 要把它们还回来。逆序析构 ⇒ view_set_ 先死、
        /// 本成员还在。此前两者顺序正好相反,于是 SceneViewSet 的析构**不可能**做
        /// 清理(它拿不到一个还活着的图缓存),只能靠外部记得调 shutdown() ——
        /// 漏了不崩,是静默泄漏。
        std::unique_ptr<SceneGraphCache> graph_cache_;

        /// 视图集合与生命周期。声明在 resources_ 与 graph_cache_ 之后:
        /// 构造需要前者(每视图 GPU 槽从其中的 SceneResources 取);后者由构造体内的
        /// setGraphCache() 接上,并由上面那条注释保证它活得比本成员久。
        SceneViewSet view_set_{resources_};
        SlotHandle scene_global_slot_{}; ///< SceneResources scene SoA slot
        FrameRetireScheduler::OwnerToken retire_owner_token_{0};

        // ── Unified transfer scheduler for scene-level uploads ──────────
        TransferScheduler transfer_scheduler_;

        // ── Stored scene-global time (set via SceneCtrlSetSceneTime) ────
        float scene_time_{0.0f};
        float scene_delta_time_{0.0f};
        uint64_t scene_total_frames_{0};
        FrameStamp current_stamp_{};
        // Domain-neutral instance cull-enable mask GPU address for the current
        // frame (0 = none). Set by a provider feature in onFrameBegin, reset to 0
        // at beginFrame start. See setInstanceCullMaskAddress().
        uint64_t instance_cull_mask_addr_{0};

        bool initialized_{false};
        std::string debug_name_;

        // (活跃视图 / 启用特性的稠密缓存与脏标记,已分别随各自集合迁入
        //  SceneViewSet / RenderFeatureSet。)
        bool recompile_suppressed_{false};
    };

    // (forEachFeature / forEachEnabledFeature 两个自由模板已删。
    //  forEachEnabledFeature 本就零调用者;forEachFeature 唯一的调用者是随
    //  MaterialPipeline 一并删除的 configureMaterialPipelines 遍历。现役遍历直接
    //  用 feature_set_.all() / .enabled() —— 后者已按启用态筛好,不需要再套一层
    //  逐个判 isEnabled 的包装。)

} // namespace lux::render
