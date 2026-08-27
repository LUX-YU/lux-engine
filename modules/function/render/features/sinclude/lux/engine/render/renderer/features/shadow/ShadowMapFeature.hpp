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
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/function/render/client/resources/lighting/EShadowTechnique.hpp>
#include <lux/engine/render/renderer/features/shadow/IShadowTechnique.hpp>
#include <lux/engine/function/render/client/resources/lighting/ShadowMapTypes.hpp>
#include <lux/engine/function/render/client/features/shadow/ShadowQualityParams.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <cstdint>
#include <string>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/function/visibility.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace lux::render
{
    struct View;
    class LightResources;
    class ViewCameraResource;
    class ShadowResources;
    class EVSMShadowResources;

    class LUX_FUNCTION_PUBLIC ShadowMapFeature : public RenderFeature
    {
    public:
        struct ShadowConfig
        {
            /// ⚠️ 这两个句柄**当前无人读取**(全类零消费点)。caster 的
            /// vert/frag 由 IShadowTechnique::casterVertVariant/FragVariant 决定,
            /// 与这里无关;它们命名的 `mesh_shadow.vert` / `shadow_depth.frag`
            /// 也已随属性路径一起删除(现为 `*_vp.vert` 的 bindless 池路径)。
            /// 保留仅为线协议布局不变 —— 下次 ABI 大版本一并摘掉。
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            uint32_t atlas_page_resolution = kDefaultShadowAtlasPageResolution;
            uint32_t atlas_page_count = kDefaultShadowAtlasPageCount;
            uint32_t max_shadow_slices = kDefaultMaxShadowSlices;
            uint32_t enable_directional_csm{0};               // 0: single-slice directional shadow, 1: CSM
            float non_directional_shadow_max_distance{60.0f}; // <=0: no distance limit
            /// Initial shadow technique. Runtime switch via setActiveTechnique().
            EShadowTechnique default_technique{EShadowTechnique::PCF};
            /// EVSM-specific knobs. Ignored under PCF.
            // RGBA16F-safe; see ShadowMapOperation.hpp comment.
            float evsm_pos_exponent{5.0f};
            float evsm_neg_exponent{5.0f};
            /// 0.5 = aggressive light-leak clamp. With fp16 exponents capped
            /// at 5 (vs Frostbite's 40), Chebyshev's depth-discrimination is
            /// loose; lower bleed_reduction values (≤0.3) leave most shadows
            /// invisibly faint. Tune up to 0.7 for hard shadows, down toward
            /// 0.2 for soft shadows on small/distant geometry.
            float evsm_bleed_reduction{0.5f};
            uint32_t evsm_atlas_page_count{4};
        };

        struct Config
        {
            ShadowConfig shadow_config{};
            std::string shadow_atlas{"ShadowAtlas"};
            std::string sync_buffer{"ShadowViewUploadSync"};
        };

        explicit ShadowMapFeature(Config cfg);
        ~ShadowMapFeature() override;

        ShadowMapFeature(const ShadowMapFeature&) = delete;
        ShadowMapFeature& operator=(const ShadowMapFeature&) = delete;

        /// 本特性的稳定身份。本代码库不开 RTTI/dynamic_cast,兄弟特性之间靠名字
        /// 相认(见 DeferredLightingFeature 的 attach 期交叉校验),所以这个名字
        /// 是**跨特性的契约而不是一句显示文本**。导出成常量:使用方引用它而不是
        /// 各自敲一遍字面量,改名时编译器会跟着走;敲字面量的话改名不会有任何提示,
        /// 只会让相认静悄悄失败,而认不出来就等于那处校验没有了。
        static constexpr std::string_view kFeatureName = "ShadowMap";

        std::string_view name() const override
        {
            return kFeatureName;
        }

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
            float non_directional_shadow_max_distance
        );
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
        [[nodiscard]] EShadowTechnique activeTechnique() const noexcept
        {
            return active_technique_;
        }
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
        [[nodiscard]] void* paramData() noexcept override
        {
            return &params_;
        }
        [[nodiscard]] std::size_t paramSize() const noexcept override
        {
            return sizeof(ShadowQualityParams);
        }
        EParamApply applyParams(const void* src, std::size_t size) override;

    private:
        struct PerViewShadowState
        {
            std::vector<ShadowSliceGPU> slices;
            std::vector<int32_t> spot_shadow_slice_index;
            std::vector<int32_t> point_shadow_base_slice;
            ShadowConfigGPU config{};
            /// 首次重建标记:churn 诊断只在「上次重建的结果」存在时比较 ——
            /// 没有它,首建会被当成一次「0 → N」的假变动。(诊断状态从
            /// static thread_local 搬进 per-view state:那份被渲染线程上
            /// 所有场景的所有 view 共享,预览世界一次重建就替主场景报假账。)
            bool built_once{false};
        };

        /// Fingerprint for incremental rebuild — if camera + lights haven't
        /// changed since last frame we can reuse the previous slice set.
        struct PerViewShadowFingerprint
        {
            Eigen::Matrix4f view_proj = Eigen::Matrix4f::Zero();
            Eigen::Vector3f camera_pos = Eigen::Vector3f::Zero();
            uint64_t light_config_hash{0};
            uint32_t shadow_config_serial{0};
        };

        void buildSlicesForView(const View& view, LightResources* light_res, PerViewShadowState& out_state);
        /// 写 EVSM 的 b9(模糊后图集)+ b10(配置 UBO)进 Light 段的域集。
        /// 在 PCF 与 EVSM 资源都 init 完之后调用一次。
        ///
        /// ⚠️ 历史教训(仍然适用,只是形态变了):渲染图编译器对 FEATURE 域槽
        /// 发的是**域集**绑定(RenderGraphCompiler::computeDescriptorBindingPlan)。
        /// 这两条曾经只写 legacy per-set 集、漏了域副本,于是着色器从域集的
        /// `offset+9 / offset+10` 读到从没被写过的 binding —— 而 PARTIALLY_BOUND
        /// 让空 binding 完全合法,EVSM 阴影**静默消失、零 validation 错误**
        /// (`6a0a3c0`)。
        ///
        /// 拆掉 legacy 半边后域集成了唯一写目标,这类"写了一半"的不对称
        /// 从形状上不再可能;但**域集为空**仍会让写入循环零次(同样静默),
        /// 那一层由 DomainWriteTarget 的接收端自查兜住。
        ///
        /// @param domain_sets            Light 所在 FEATURE 域的 per-FIF 集
        /// @param domain_binding_offset  Light 在域内的起始偏移(= +2,跳过 Instance 的两条)
        void writeEVSMBindings(
            LightResources& light_res,
            EVSMShadowResources& evsm_res,
            std::span<const VkDescriptorSet> domain_sets,
            uint32_t domain_binding_offset
        );
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

        /// attach 期缓存,恒非空:本 feature 声明了 requires=lux.render.light.v1
        /// (见 ShadowMapOperation.hpp 的 LUX_COMM_CONFIG 注解),所以 beginInstall
        /// 保证 LightFeature 先装、LightResources 已在场景注册表里。原先是每帧
        /// onFrameBegin 重查一次 + 判空。
        LightResources* light_res_ = nullptr;
        /// 可选相机资源的记忆化缓存(见 resolveViewCameraOnce 的注释)。
        ViewCameraResource* cam_cache_ = nullptr;

        // Per-view computed slices for this frame (key = View::handle)
        lux::cxx::BasicSparseSet<uint32_t, PerViewShadowState> per_view_shadow_;
        // Per-view fingerprints for incremental rebuild (key = View::handle)
        lux::cxx::BasicSparseSet<uint32_t, PerViewShadowFingerprint> per_view_fingerprint_;
        // Cached slice data from previous frame for reuse (key = View::handle)
        lux::cxx::BasicSparseSet<uint32_t, PerViewShadowState> prev_view_shadow_;

        // Shadow technique polymorphism — owned at the feature level so
        // setActiveTechnique() can flip the active impl without touching
        // per-view state. C2: only `id()` / `lightingFragVariant*()` are
        // consulted; C3+ progressively routes per-frame caster / post
        // dispatch through `currentTechnique()`. See IShadowTechnique.hpp.
        std::array<std::unique_ptr<IShadowTechnique>, kShadowTechniqueCount> techniques_;
        EShadowTechnique active_technique_{EShadowTechnique::PCF};
    };

} // namespace lux::render
