#pragma once
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>
#include <lux/engine/render/RenderContextView.hpp>  // contextView() return type (narrow facade)
#include <lux/engine/render/RenderSceneView.hpp>    // sceneView() return type (narrow facade)
#include <lux/engine/function/render/client/core/FeatureHandle.hpp> // FeatureHandle (generational)
#include <lux/engine/function/render/client/core/FeatureTypeId.hpp> // FeatureTypeId (stable type identity)
#include <lux/engine/function/render/client/core/FeatureDescriptor.hpp> // FeatureDescriptor (type-level metadata)
#include <lux/engine/function/render/client/core/Errors.hpp>            // Expected<void> (attach result)
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::render
{
    class RGBuilder;
    class RenderContext;
    class RenderScene;
    struct RGFrameContext;           // Renderer passes this for ext-data injection

    // =========================================================================
    // Per-frame feature contexts
    // =========================================================================
    struct FeatureFrameContext
    {
        uint32_t           frame_index{0};
    };

    // =========================================================================
    // FeatureState — lifecycle state of an installed feature
    // =========================================================================
    // Transitions are driven by RenderScene (install / setEnabled / remove); a
    // feature never mutates its own state. The transient states (Attaching/
    // Enabling/Disabling/Detaching) mark in-progress steps so a failed transactional
    // install can roll back to a known point (slice 3c). Failed marks a feature whose
    // attach failed — it is detached, never enabled.
    enum class FeatureState : std::uint8_t
    {
        Constructed,  ///< object built, not yet attached to a scene
        Attaching,    ///< initAndAttachTo in progress
        Disabled,     ///< attached, not contributing this frame
        Enabling,     ///< Disabled → Enabled in progress
        Enabled,      ///< attached + contributing
        Disabling,    ///< Enabled → Disabled in progress
        Detaching,    ///< being removed from the scene
        Failed,       ///< attach failed; awaiting detach
    };

    // =========================================================================
    // RenderFeature — one renderer capability installed into a RenderScene
    // =========================================================================

    /**
     * @brief One renderer capability installed into a RenderScene.
     *
     * It owns its renderer lifetime, state and optional graph contribution:
     *   - 生命周期:initAndAttachTo / onDetachFromScene
     *   - 每视图状态:allocateViewState / deallocateViewState
     *   - 每帧回调:onFrameBegin / populateFrameContext
     *   - 可调参数:paramStructName / paramData / applyParams
     *   - 身份与状态:featureId / typeId / descriptor / featureState
     *
     * Resource-only capabilities use the default empty addPasses(). Pass
     * producers override it. This keeps one feature identity and one lifecycle
     * instead of creating a parallel base type solely for a no-op method.
     */
    class LUX_FUNCTION_PUBLIC RenderFeature
    {
    public:
        struct Config
        {
            std::string              name;
        };

        RenderFeature(Config cfg)
            : cfg_(std::move(cfg)){}

        virtual ~RenderFeature() = default;

        /// Globally unique name (e.g. "DepthPrepass", "ForwardMesh", "SSAO").
        virtual std::string_view name() const
        {
            return cfg_.name;
        }
        // --- Scene lifecycle ---

        /// Render-thread only. Complete all GPU initialisation and bind to
        /// the owning scene.  Called by RenderScene when the feature is
        /// first added (or when a deferred add arrives via FramePack).
        /// The scene/context accessors are valid inside this callback.
        /// Returns an error to ABORT the install: RenderScene::addFeatureImpl
        /// drops the half-attached feature and the add fails. Default = success.
        virtual Expected<void> initAndAttachTo(RenderScene& /*scene*/) { return {}; }

        /// Called when this feature is removed from a RenderScene.
        virtual void onDetachFromScene(RenderScene& /*scene*/) {}

        /// Owning scene/context (valid after initAndAttachTo).
        ///
        /// contextView()/sceneView() are the NARROW, (eventually) external-facing
        /// SDK surface — a feature sees only the curated RenderContextView /
        /// RenderSceneView (lightweight views, returned by value). In-module
        /// "engine-subsystem" features (GPU-driven mesh, deferred, shadow, …) that
        /// legitimately need the full concrete API use renderContext()/renderScene()
        /// instead (RenderContext / RenderScene are incomplete outside the render
        /// module, so those accessors are unusable there).
        [[nodiscard]] RenderContextView contextView() noexcept;
        [[nodiscard]] RenderSceneView   sceneView() noexcept;

        [[nodiscard]] RenderContext& renderContext() noexcept;
        [[nodiscard]] const RenderContext& renderContext() const noexcept;
        [[nodiscard]] RenderScene& renderScene() noexcept;
        [[nodiscard]] const RenderScene& renderScene() const noexcept;

        // --- Per-view GPU state ---

        /// Allocate GPU resources for a newly created view.
        /// Return false on failure: RenderScene then records NO per-view state for
        /// this (feature, view) pair and will not call deallocateViewState() for
        /// it. Default: nothing to allocate, always succeeds.
        virtual bool allocateViewState(uint32_t /*view*/, RenderScene& /*scene*/) { return true; }

        /// Release GPU resources for a view. Called exactly once per successful
        /// allocateViewState() — on view removal OR when the feature itself is
        /// removed from the scene. Must not throw.
        virtual void deallocateViewState(uint32_t /*view*/) {}

        // --- Per-frame callbacks --------------------------------------------

        /// Phase 1: process per-frame logic using scene-wide primitive contexts.
        virtual void onFrameBegin(const FeatureFrameContext& /*ctx*/) {}

        /// Rare render-safe-point transaction used when the fixed scene page
        /// origin can no longer represent a newly active object. RenderScene
        /// runs the validation hook for every installed feature before it lets
        /// any feature mutate state, so a rejected rebase is never half-applied.
        [[nodiscard]] virtual bool canRebaseSceneOrigin(
            const std::int64_t /*origin_delta*/[3]) const noexcept
        {
            return true;
        }
        virtual void rebaseSceneOrigin(
            const std::int64_t /*origin_delta*/[3]) noexcept
        {}

        // (onFrustumUpdated removed — now that View is no longer inherently 3D, the
        // per-view frustum is a feature domain. The cull / point-cloud features pull it
        // from ViewCameraResource; the core no longer broadcasts a Frustum to every
        // enabled feature.)

        // (configureMaterialPipelines + the whole MaterialPipeline class are gone.
        //  It was a hook with neither producer nor consumer: zero features ever
        //  overrode it — including ForwardMeshFeature, which its own doc comment
        //  named as the expected implementor — and MaterialPipeline's setters had
        //  no reader anywhere, so even an override would have written into a value
        //  nothing ever read. RenderScene paid two full feature-loop passes for it.
        //  The live material path is MaterialResources +
        //  VariantBucketManager; per-family pipelines are registered through
        //  GpuDrivenMeshFeatureBase::registerFamilyPipelines.)

        /// Inject feature-owned per-frame data into the frame context.
        /// Called by Renderer::renderView() for each enabled feature,
        /// before graph recording.  Default is a no-op.
        virtual void populateFrameContext(RGFrameContext& /*frame_ctx*/) {}

        /// Contribute render-graph passes. Resource-only capabilities keep the
        /// default no-op; pass-producing capabilities override it.
        virtual void addPasses(RGBuilder& /*builder*/) {}

        /// 本特性需要渲染目标提供哪些**额外输出语义槽**
        /// (TargetSlot 位掩码,bit = `1u << static_cast<uint32_t>(slot)`)。
        ///
        /// 生产者与消费者都在此声明 —— LinearDepth 的解算特性与采样它的
        /// SSAO 特性都返回 LinearDepth 位。场景聚合并集,编排器把缺的槽按
        /// 默认描述(graph/RenderTargetLayout.hpp 的 defaultTargetSlotDesc)
        /// 写进目标布局;布局变更经既有 format-key 比较自动触发图模板重编
        /// 与池重建 —— 特性侧只声明"要什么",不碰"在哪/怎么建"。
        ///
        /// 默认 0 = 不需要额外槽。
        [[nodiscard]] virtual uint32_t requiredTargetSlots() const { return 0; }

        // --- Runtime enable/disable -----------------------------------------
        // Single source of truth: enabled-ness DERIVES from the lifecycle state,
        // which only RenderScene::setFeatureEnabled (the handle path) drives. There is
        // no separate enabled_ flag that could diverge from it.
        [[nodiscard]] bool isEnabled() const noexcept { return lifecycle_state_ == FeatureState::Enabled; }

        // --- Tunable parameters (feature-driven quality system) -------------
        //
        // OPT-IN seam: a feature MAY expose a reflectable, live-tunable parameter
        // struct (runtime TypeInfo with LUX_MEMBER fields) so the editor's rendering-
        // settings panel + the quality-tier system can enumerate and drive it
        // generically — no per-feature UI code. The empty defaults below keep
        // every existing feature compiling unchanged; a feature opts in by
        // overriding all three. See
        // .internal/feature-quality-tiers-design-2026-06-19.md.

        /// How costly applying a param snapshot was, so the scene/driver can
        /// decide whether to force a graph recompile or accept a free hot-apply.
        enum class EParamApply : uint8_t
        {
            UNSUPPORTED,      ///< feature exposes no params (default)
            HOT,              ///< applied live; nothing to rebuild, visible next frame
            NEEDS_RECOMPILE,  ///< caller must RenderScene::invalidateGraph()
            NEEDS_RECREATE    ///< feature recreated GPU resources internally (GPU-idle)
        };

        /// Fully-qualified reflected type name of this feature's param struct,
        /// or "" if it exposes none. The editor calls ReflectionRegistry::
        /// findClass(name) to enumerate the editable fields.
        [[nodiscard]] virtual std::string_view paramStructName() const { return {}; }

        /// Pointer to the LIVE param-struct instance (render-thread-owned storage
        /// the feature reads each frame), or nullptr. The editor edits it in place
        /// via the reflected field layout for hot preview.
        [[nodiscard]] virtual void* paramData() noexcept { return nullptr; }

        /// Byte size of the struct paramData() points at, or 0 if none. The
        /// feature is the SOLE size authority: the render module has no reflection
        /// sidecar (editor-only), so the server cannot derive it. The editor uses
        /// this to snapshot the current param bytes for enumeration.
        [[nodiscard]] virtual std::size_t paramSize() const noexcept { return 0; }

        /// Apply a whole param-struct snapshot (already name-matched by the
        /// caller). The feature diffs against its current state and returns the
        /// STRONGEST verdict (hot + rebuild fields can coexist in one snapshot).
        virtual EParamApply applyParams(const void* /*src*/, std::size_t /*size*/)
        {
            return EParamApply::UNSUPPORTED;
        }

        // --- Feature ID (assigned by RenderScene during registration) ------

        [[nodiscard]] FeatureHandle featureId() const noexcept { return feature_id_; }

        /// Stable type identity (== descriptor().type), set by RenderScene at
        /// add-time from the FeatureFactory descriptor. kInvalidFeatureTypeId for
        /// features whose factory declared none — they skip dependency/conflict checks.
        [[nodiscard]] FeatureTypeId typeId() const noexcept { return descriptor_.type; }

        /// Static type-level metadata (deps / conflicts / capability flags), copied
        /// from the FeatureFactory at add-time. Default-empty if none was declared.
        [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept { return descriptor_; }

        /// Current lifecycle state (managed by RenderScene; see FeatureState).
        [[nodiscard]] FeatureState featureState() const noexcept { return lifecycle_state_; }

    protected:
        RenderFeature() = default;
        // (No enabled_ flag — enabled-ness is lifecycle_state_ == Enabled; see isEnabled.
        //  A feature is added Enabled by RenderScene::addFeatureImpl and toggled only by
        //  RenderScene::setFeatureEnabled, which owns the FeatureState transitions.)

    private:
        Config                          cfg_;
        RenderScene*                    scene_{nullptr};

        friend class RenderScene;
        FeatureHandle                   feature_id_{};
        FeatureDescriptor               descriptor_{};                       ///< type-level metadata
        FeatureState                    lifecycle_state_{FeatureState::Constructed};
    };

} // namespace lux::render
