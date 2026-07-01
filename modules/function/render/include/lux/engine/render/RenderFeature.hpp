#pragma once
#include <lux/engine/render/graph/RGForwardDecls.hpp>
#include <lux/engine/render/RenderContextView.hpp>  // contextView() return type (narrow facade)
#include <lux/engine/render/RenderSceneView.hpp>    // sceneView() return type (narrow facade)
#include <lux/engine/render/core/FeatureHandle.hpp> // FeatureHandle (generational)
#include <lux/engine/render/core/FeatureTypeId.hpp> // FeatureTypeId (stable type identity)
#include <lux/engine/render/core/FeatureDescriptor.hpp> // FeatureDescriptor (type-level metadata)
#include <lux/engine/render/core/Errors.hpp>            // Expected<void> (attach result, 三-3)
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
    class MaterialPipeline;
    struct RGFrameContext;           // Renderer passes this for ext-data injection
    struct ViewFamily;

    // =========================================================================
    // Per-frame feature contexts
    // =========================================================================
    struct FeatureFrameContext
    {
        uint32_t           frame_index{0};
    };

    // =========================================================================
    // FeatureState — lifecycle state of an installed feature (阶段 3)
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
    // RenderFeature — abstract base class
    // =========================================================================

    /**
     * @brief A self-contained rendering capability that can be registered with
     *        RenderScene at runtime.
     *
     * Each RenderFeature is responsible for:
     *   1. Loading its own shaders and registering pipeline templates
     *   2. Adding its render-graph passes
     *   3. Creating and registering its own PassProcessor(s)
     *   4. (Optional) Initialising feature-specific GPU resources
     *
     * Built-in features (DepthPrepass, ForwardMesh, Grid, PointCloud) use
     * this same interface, so user-defined features have identical power.
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
        /// Returns an error to ABORT the install (三-3): RenderScene::addFeatureImpl
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

        /**
         * @brief Add render-graph passes to the builder.
         *
         * Called once during RenderGraphBuildService::build().  The feature
         * declares its reads/writes via builder.referenceTexture()/referenceBuffer()
         * and any transients it creates.
         */
        virtual void addPasses(RGBuilder& builder) = 0;

        // --- Per-frame callbacks --------------------------------------------

        /// Phase 1: process per-frame logic using scene-wide primitive contexts.
        virtual void onFrameBegin(const FeatureFrameContext& /*ctx*/) {}

        // (onFrustumUpdated removed — View 去 3D 化: the per-view frustum is a feature
        // domain now. The cull / point-cloud features pull it from ViewCameraResource;
        // the core no longer broadcasts a Frustum to every enabled feature.)

        /**
         * @brief Wire this feature's material pipeline handles into the shared MaterialPipeline.
         *
         * Called once after all features are constructed.
         * The default is a no-op; features that own material-type pipeline handles
         * (e.g. ForwardMeshFeature) should override this.
         */
        virtual void configureMaterialPipelines(MaterialPipeline& /*mp*/) {}

        /// Inject feature-owned per-frame data into the frame context.
        /// Called by Renderer::renderView() for each enabled feature,
        /// before graph recording.  Default is a no-op.
        virtual void populateFrameContext(RGFrameContext& /*frame_ctx*/) {}

        // --- Runtime enable/disable -----------------------------------------
        // Single source of truth (三-1): enabled-ness DERIVES from the lifecycle state,
        // which only RenderScene::setFeatureEnabled (the handle path) drives. There is
        // no separate enabled_ flag that could diverge from it.
        [[nodiscard]] bool isEnabled() const noexcept { return lifecycle_state_ == FeatureState::Enabled; }

        // --- Tunable parameters (feature-driven quality system) -------------
        //
        // OPT-IN seam: a feature MAY expose a reflectable, live-tunable parameter
        // struct (a LUX_CLASS with LUX_MEMBER fields) so the editor's rendering-
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
        [[nodiscard]] uint32_t extractorTypeId() const noexcept { return extractor_type_id_; }

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
        //  RenderScene::setFeatureEnabled, which owns the FeatureState transitions. 三-1)

    private:
        Config                          cfg_;
        RenderScene*                    scene_{nullptr};

        friend class RenderScene;
        FeatureHandle feature_id_{};
        uint32_t extractor_type_id_{0};
        FeatureDescriptor descriptor_{};                       ///< type-level metadata (阶段 3)
        FeatureState lifecycle_state_{FeatureState::Constructed};
    };

} // namespace lux::render
