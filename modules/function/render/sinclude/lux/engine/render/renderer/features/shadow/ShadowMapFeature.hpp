#pragma once
/**
 * @file ShadowMapFeature.hpp
 * @brief Independent shadow map rendering feature — runtime-switchable
 *        directional shadow mode (single-slice or CSM), plus standard
 *        2D shadow maps for spot lights.
 *
 * GPU resource ownership (atlas, SSBO, UBO, descriptor set) lives in
 * ShadowResources (GPUResourceRegistry).
 *
 * This feature retains:
 *   - Per-view slice computation (buildSlicesForView)
 *   - Shadow DS layout creation
 *   - Render-graph atlas import
 */

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/core/EShadowTechnique.hpp>
#include <lux/engine/render/renderer/features/shadow/IShadowTechnique.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapTypes.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowQualityParams.hpp>
#include <lux/engine/render/scene/ViewStateTable.hpp>
#include <cstdint>
#include <string>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/function/visibility.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace lux::render
{
    struct View;
    class LightResources;
    class ShadowResources;
    class EVSMShadowResources;

    class LUX_FUNCTION_PUBLIC ShadowMapFeature : public RenderFeature
    {
    public:
        struct ShadowConfig
        {
            ShaderHandle vertex_shader{};     // mesh_shadow.vert
            ShaderHandle fragment_shader{};   // shadow_depth.frag
            uint32_t     atlas_page_resolution = kDefaultShadowAtlasPageResolution;
            uint32_t     atlas_page_count      = kDefaultShadowAtlasPageCount;
            uint32_t     max_shadow_slices     = kDefaultMaxShadowSlices;
            uint32_t     enable_directional_csm{0}; // 0: single-slice directional shadow, 1: CSM
            float        non_directional_shadow_max_distance{60.0f}; // <=0: no distance limit
            /// Initial shadow technique. Runtime switch via setActiveTechnique().
            EShadowTechnique default_technique{EShadowTechnique::PCF};
            /// EVSM-specific knobs. Ignored under PCF.
            // RGBA16F-safe; see ShadowMapOperation.hpp comment.
            float    evsm_pos_exponent{5.0f};
            float    evsm_neg_exponent{5.0f};
            /// 0.5 = aggressive light-leak clamp. With fp16 exponents capped
            /// at 5 (vs Frostbite's 40), Chebyshev's depth-discrimination is
            /// loose; lower bleed_reduction values (≤0.3) leave most shadows
            /// invisibly faint. Tune up to 0.7 for hard shadows, down toward
            /// 0.2 for soft shadows on small/distant geometry.
            float    evsm_bleed_reduction{0.5f};
            uint32_t evsm_atlas_page_count{4};
        };

        struct Config
        {
            ShadowConfig shadow_config{};
            std::string  shadow_atlas{"ShadowAtlas"};
            std::string  sync_buffer{"ShadowViewUploadSync"};
        };

        explicit ShadowMapFeature(Config cfg);
        ~ShadowMapFeature() override;

        ShadowMapFeature(const ShadowMapFeature&) = delete;
        ShadowMapFeature& operator=(const ShadowMapFeature&) = delete;

        std::string_view name() const override { return "ShadowMap"; }

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;

        /// Runtime shadow quality update (called from operation handler).
        /// Integer fields: value 0 means "keep current".
        /// non_directional_shadow_max_distance: <0 keeps current, 0 disables limit.
        /// Returns true if atlas config actually changed and resources were rebuilt.
        [[nodiscard]] bool updateQuality(
            uint32_t atlas_page_resolution,
            uint32_t atlas_page_count,
            uint32_t max_shadow_slices,
            float non_directional_shadow_max_distance);
        /// Runtime switch for directional shadow mode.
        /// false: force one directional slice, true: use configured cascades.
        void setDirectionalCsmEnabled(bool enabled);

        /// Switch the active shadow technique at runtime (PCF / EVSM / ...).
        /// C2 behavior: the enum is stored and the active technique's
        /// `lightingFragVariant*()` is published via `currentTechnique()` so
        /// DeferredLightingFeature picks the right SPIR-V variant on its
        /// next rebuild. Resource / pass dispatch routing through the active
        /// technique progressively lands in C3-C5.
        void setActiveTechnique(EShadowTechnique technique) noexcept;
        [[nodiscard]] EShadowTechnique activeTechnique() const noexcept { return active_technique_; }
        [[nodiscard]] IShadowTechnique& currentTechnique() noexcept;
        [[nodiscard]] const IShadowTechnique& currentTechnique() const noexcept;

        // --- Feature-driven quality seam (reflected ShadowQualityParams) -------
        // The quality knobs route through the shared FeatureParams op: the editor
        // edits params_ in place (paramData) and the shared handler hands a full
        // snapshot to applyParams, which delegates to updateQuality /
        // setDirectionalCsmEnabled. An atlas-resolution/count/slice change rebuilds
        // GPU resources → NEEDS_RECOMPILE; the CSM toggle is a hot per-frame read.
        [[nodiscard]] std::string_view paramStructName() const override
        {
            return "lux::render::ShadowQualityParams";
        }
        [[nodiscard]] void* paramData() noexcept override { return &params_; }
        [[nodiscard]] std::size_t paramSize() const noexcept override { return sizeof(ShadowQualityParams); }
        EParamApply applyParams(const void* src, std::size_t size) override;

    private:
        struct PerViewShadowState
        {
            std::vector<ShadowSliceGPU> slices;
            std::vector<int32_t>        spot_shadow_slice_index;
            std::vector<int32_t>        point_shadow_base_slice;
            ShadowConfigGPU             config{};
        };

        /// Fingerprint for incremental rebuild — if camera + lights haven't
        /// changed since last frame we can reuse the previous slice set.
        struct PerViewShadowFingerprint
        {
            Eigen::Matrix4f  view_proj = Eigen::Matrix4f::Zero();
            Eigen::Vector3f  camera_pos = Eigen::Vector3f::Zero();
            uint64_t         light_config_hash{0};
            uint32_t         shadow_config_serial{0};
        };

        void buildSlicesForView(const View& view, LightResources* light_res, PerViewShadowState& out_state);
        /// C3c step 4: write EVSM bindings 9 (blurred atlas) + 10 (config UBO)
        /// into the per-FIF light descriptor sets. Called once after both
        /// PCF and EVSM resources are initialized.
        void writeEVSMBindings(LightResources& light_res, EVSMShadowResources& evsm_res);
        const PerViewShadowState* resolveViewState(uint32_t view_handle) const;
        uint64_t computeLightConfigHash(LightResources* light_res) const;

        bool initialized_{false};
        Config cfg_{};
        /// Live mirror of the quality knobs for the editor settings panel
        /// (paramData). Initialized from cfg_ at construction; applyParams refreshes
        /// it and drives the real state via updateQuality / setDirectionalCsmEnabled.
        ShadowQualityParams params_{};
        uint32_t shadow_config_serial_{0}; ///< Bumped on updateQuality / setDirectionalCsmEnabled

        VkDevice device_ = VK_NULL_HANDLE;

        // DS layout (owned here — needed for pipeline layout construction)
        DescriptorLayoutId shadow_ds_layout_id_{kInvalidDescriptorLayoutId};

        // Non-owning pointer to ShadowResources (retrieved from GPUResourceRegistry)
        ShadowResources* shadow_res_ = nullptr;

        // Per-view computed slices for this frame (key = View::handle)
        ViewStateTable<PerViewShadowState> per_view_shadow_;
        // Per-view fingerprints for incremental rebuild (key = View::handle)
        ViewStateTable<PerViewShadowFingerprint> per_view_fingerprint_;
        // Cached slice data from previous frame for reuse (key = View::handle)
        ViewStateTable<PerViewShadowState> prev_view_shadow_;

        // Shadow technique polymorphism — owned at the feature level so
        // setActiveTechnique() can flip the active impl without touching
        // per-view state. C2: only `id()` / `lightingFragVariant*()` are
        // consulted; C3+ progressively routes per-frame caster / post
        // dispatch through `currentTechnique()`. See IShadowTechnique.hpp.
        std::array<std::unique_ptr<IShadowTechnique>, kShadowTechniqueCount> techniques_;
        EShadowTechnique active_technique_{EShadowTechnique::PCF};
    };

} // namespace lux::render
