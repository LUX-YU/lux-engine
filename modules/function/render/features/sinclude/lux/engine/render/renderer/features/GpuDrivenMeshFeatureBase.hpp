#pragma once
#include <lux/engine/function/render/client/features/GpuDrivenMeshExtFlags.hpp>
/**
 * @file GpuDrivenMeshFeatureBase.hpp
 * @brief Common base class for GPU-driven mesh render features.
 *
 * Owns all shared state and provides lifecycle and
 * buffer-management methods.  Concrete subclasses (ForwardMeshFeature,
 * DeferredGBufferFeature) override only the pipeline-specific parts:
 *   - addPasses()            — view cull + draw passes
 *   - initAndAttachTo()      — feature-specific init, pipeline registration
 */

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/resources/mesh/GpuDrivenMeshConsts.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/mesh/MdcTable.hpp>
#include <lux/engine/render/gpu/pipeline/ShaderPermutation.hpp>
#include <lux/engine/render/gpu/ShaderObject.hpp>
#include <lux/engine/render/gpu/VmaFwd.hpp>
#include <lux/engine/function/render/client/core/VertexLayoutTypes.hpp>   // VertexLayoutId
#include <lux/engine/render/resources/material/MaterialFamily.hpp>      // EShadingModel
#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/render/resources/mesh/MeshInstanceExtData.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>            // phaseBit / ECoreRenderPhase
#include <lux/engine/function/render/client/resources/mesh/RenderObjectTypes.hpp>   // EPassDomain / PassMask
#include <lux/engine/function/visibility.h>
#include <lux/engine/gapi/vk/Pipeline.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace lux::render
{
    class MeshResources;
    class RGBuilder;
    class VertexLayoutRegistry;
    struct GraphicsPipelineTemplate;

    // =========================================================================
    //  GpuDrivenMeshFeatureBase
    // =========================================================================
    class LUX_FUNCTION_PUBLIC GpuDrivenMeshFeatureBase : public RenderFeature
    {
    public:

        /// Direct access to the three-stream instance storage.
        InstanceResources *instanceStorage() noexcept { return instance_res_; }

        // =====================================================================
        //  RenderFeature lifecycle (common implementations)
        // =====================================================================
        void onFrameBegin(const FeatureFrameContext &ctx) override;
        void populateFrameContext(RGFrameContext& frame_ctx) override;

    protected:
        ~GpuDrivenMeshFeatureBase() override;

        // ── Common init / destroy ──
        // Returns unexpected when a hard prerequisite is missing (InstanceResources
        // not registered by StandardMeshStack, or the view-cull shader unresolved) so
        // callers fail the install instead of half-initialising / dereferencing null.
        //
        Expected<void> initCommon(ShaderHandle view_cull_shader_id, GpuDrivenMeshExtFlags extension_flags = {});
        void destroyCommon() noexcept;

        /// 本 feature 剔除/绘制接受的几何类别掩码。全域统一(static+skinned)
        /// ——按 pass 域收窄的需求出现时再加参数,勿留摆设参。
        [[nodiscard]] uint32_t supportedGeometryMask() const noexcept;
        [[nodiscard]] static PassMask passMaskForPhase(ECoreRenderPhase phase) noexcept;

        /// Creates compact compute pipeline (per-MDC, replaces finalize).
        /// propagates layout/shader failures instead of asserting-and-
        /// continuing (a release build would leave compact_pipeline_ invalid and the
        /// compact pass would silently record nothing — indirect draws never compacted).
        [[nodiscard]] Expected<void> initCompactPipeline(ShaderHandle compact_shader_id, std::string_view debug_name);

        /// Adds a per-MDC compact pass (replaces finalize for view path).
        void addCompactPass(
            RGBuilder& builder,
            std::string_view pass_name,
            std::string_view after_pass,
            RGTransientDSHandle cull_tds);

        /// The per-feature knobs of the shared cull+compact construction — the
        /// only things that differ between the Forward and Deferred view paths.
        struct CullCompactParams
        {
            std::string_view prefix;             ///< RG debug-name prefix ("Fwd" / "DeferredGBuf")
            ECoreRenderPhase phase;              ///< drives the cull kernel pass_mask
            EPassDomain      domain;             ///< drives the cull kernel geometry_mask
            std::string_view cull_pass_name;     ///< e.g. "ForwardMeshForwardCull"
            std::string_view compact_pass_name;  ///< e.g. "ForwardMeshForwardCompact"
            uint32_t         descriptor_layout_version{0};
            GpuDrivenMeshExtFlags         extension_flags{};
            //(原有 condition / condition_tag 两个字段已删:整链跳过现在由调用方
            // 用 RGBuilder::conditionChain 开一个作用域,本函数建的 pass 经
            // builder.addPass 出来即自动入链,不必再逐个把条件传进来。)
        };

        /// Build the shared RG-transient cull + compact passes for a GPU-driven
        /// mesh VIEW path: rebuilds MDC offsets, creates the indirect/count/visible
        /// + frustum buffers, imports the instance buffers, builds the 8-binding
        /// cull transient DS, and registers the MeshCull + MdcCompact passes. Sets
        /// the base RG-handle members (draw_indirect_rg_ / draw_count_rg_ /
        /// visible_instance_rg_ / mdc_info_rg_) for the caller's draw pass.
        /// Shared by ForwardMeshFeature + DeferredGBufferFeature (H9 de-dup);
        /// MeshShadowFeature keeps its own shadow-specific variant.
        void addCullAndCompactPasses(RGBuilder& builder, const CullCompactParams& p);

        using InstanceBufferGetter =
            VkBuffer (InstanceResources::*)() const noexcept;

        /// Import one stable InstanceResources buffer with the common storage
        /// contract. View, shadow, highlight and feedback paths must describe
        /// the same physical buffer identically to the RenderGraph.
        [[nodiscard]] RGResourceHandle importInstanceStorageBuffer(
            RGBuilder& builder,
            std::string_view name,
            VkDeviceSize size,
            std::uint32_t stride,
            std::uint32_t element_count,
            InstanceBufferGetter getter);

        /// Import every currently allocated classic-mesh IBO segment. The
        /// returned span remains stable through this graph compile and is copied
        /// into emitted bind commands as ordinary RG resource indices.
        ///
        /// 为什么由特性导入、而不是录制器去注册表里捞:索引缓冲属于**领域数据**
        /// (MeshResources,L3),录制器属于渲染图机器(L2)。让 L2 认识 MeshResources
        /// 是抽象泄露 —— 谁组装、装的是全局还是场景的资源,本就该由上层说了算。
        /// 导入后 kernel 只拿到一个通用的 RG 资源下标,经 resolveBuffer 解析,图也
        /// 因此第一次看见这条缓冲(依赖/生命周期),且分配器每帧刷句柄,网格池扩容
        /// 重分配自动跟上。
        ///
        /// 四个 GPU 驱动网格特性(前向/延迟/高亮/阴影)可任意子集启用,没有唯一
        /// 生产者,故统一用固定资源名 —— importBuffer 按名复用,首个调用者建、
        /// 其余复用同一条。
        ///
        [[nodiscard]] std::span<const RGResourceHandle>
        importSharedIndexBuffers(RGBuilder& builder);

        // ── MDC-based buffer sizing (the only path; bucket count is dynamic,
        //    bounded by mdcCount(), NOT a fixed kMaxBuckets grid) ──
        /// Rebuild MDC offsets and prepare GPU upload data.  Call once per frame.
        void buildMdcOffsets();

        /// Number of unique MDC entries in the current scene.
        [[nodiscard]] uint32_t mdcCount() const noexcept;

        /// MDC-based indirect buffer size (one IndirectCmd per MDC).
        [[nodiscard]] VkDeviceSize mdcIndirectBufferSize() const noexcept;

        /// MDC-based draw-count buffer size (one counter per MDC).
        [[nodiscard]] VkDeviceSize mdcDrawCountBufferSize() const noexcept;

        /// MDC-based visible buffer size (total capacity from MDC offsets).
        [[nodiscard]] VkDeviceSize mdcVisibleBufferSize() const noexcept;

        /// Creates or reuses the shared 9-binding cull descriptor set layout.
        void createCullLayout();

        // =====================================================================
        //  Variant-bucket pipeline resolution helpers (shared by F/D)
        // =====================================================================
        using FamilyPipelineArray = std::array<GraphicsPipelineHandle, kLightingTechniqueCount>;

        /// The pipeline pool for a GPU-driven mesh draw pass, resolved per VARIANT
        /// BUCKET. The bucket — NOT the family — is the routing unit: the draw
        /// kernel already indexes `pipeline_variants[bucket_id]` (MeshKernels.cpp,
        /// `variants[vidx]` with `vidx == bucket_id`).
        ///
        /// Two tiers, consulted in order by pick():
        ///   1. a per-bucket PSO override (`bucket_normal` / `bucket_nocull`, keyed
        ///      by bucket_id) — populated via registerBucketPipeline() when a bucket
        ///      carries its own baked shader (R1: graph materials). Empty by default.
        ///   2. the per-FAMILY bootstrap (`bootstrap_normal` / `bootstrap_nocull`,
        ///      one per ELightingTechnique), built by registerFamilyPipelines() —
        ///      the fallback when a bucket has no override of its own.
        /// With no overrides registered, pick() returns the family bootstrap EXACTLY
        /// as before (behavior-preserving until R1 starts calling
        /// registerBucketPipeline).
        struct BucketPipelinePool
        {
            // Tier 2: per-family bootstrap pipelines (back-face cull + no-cull).
            FamilyPipelineArray bootstrap_normal{};
            FamilyPipelineArray bootstrap_nocull{};

            // Tier 1: per-bucket PSO overrides, indexed by bucket_id (sized lazily;
            // empty until a bucket registers its own baked-shader pipeline).
            std::vector<GraphicsPipelineHandle> bucket_normal;
            std::vector<GraphicsPipelineHandle> bucket_nocull;

            void reset() noexcept
            {
                bootstrap_normal.fill({});
                bootstrap_nocull.fill({});
                bucket_normal.clear();
                bucket_nocull.clear();
            }

            /// Register a per-bucket PSO override (R1). Grows the override vector as
            /// needed; @p two_sided selects the DOUBLE_SIDED (no-cull) tier.
            void registerBucketPipeline(uint32_t bucket_id, bool two_sided,
                                        GraphicsPipelineHandle handle)
            {
                auto& v = two_sided ? bucket_nocull : bucket_normal;
                if (bucket_id >= v.size())
                    v.resize(bucket_id + 1u);
                v[bucket_id] = handle;
            }

            /// Resolve the pipeline for the bucket at @p bucket_id (its index in
            /// pipeline_variants): its own override if registered, else the family
            /// bootstrap (the no-cull variant when DOUBLE_SIDED). Falls back to
            /// family 0 so an empty bucket never resolves an invalid handle (it has
            /// no visible instances, so its pipeline is never actually drawn with).
            GraphicsPipelineHandle pick(uint32_t bucket_id,
                                        const VariantBucketDesc& bucket) const noexcept
            {
                const bool two_sided =
                    hasFeature(bucket.feature_mask, EShaderFeature::DOUBLE_SIDED);

                // Tier 1: per-bucket override.
                const std::vector<GraphicsPipelineHandle>& ov =
                    two_sided ? bucket_nocull : bucket_normal;
                if (bucket_id < ov.size() && ov[bucket_id].valid())
                    return ov[bucket_id];

                // Tier 2: family bootstrap.
                const FamilyPipelineArray& arr = two_sided ? bootstrap_nocull : bootstrap_normal;
                const uint32_t family_index = static_cast<uint32_t>(bucket.family);
                GraphicsPipelineHandle h =
                    (family_index < arr.size()) ? arr[family_index] : arr[0];
                if (!h.valid())
                    h = arr[0];
                return h;
            }
        };

        /// Collect material variant buckets from `mat_res`, defaulting to one
        /// bucket per family (feature_mask=0) when none have been registered yet.
        /// Mirrors the prior inline behaviour of ForwardMesh and DeferredGBuffer.
        static std::vector<MaterialResources::VariantBucketDesc>
        collectVariantBuckets(MaterialResources* mat_res)
        {
            std::vector<MaterialResources::VariantBucketDesc> buckets;
            if (mat_res != nullptr)
            {
                const uint32_t bucket_count = mat_res->variantBucketCount();
                buckets.reserve(bucket_count);
                for (uint32_t b = 0; b < bucket_count; ++b)
                    buckets.push_back(mat_res->variantBucket(b));
            }
            if (buckets.empty())
            {
                buckets.reserve(kLightingTechniqueCount);
                for (uint32_t fi = 0; fi < kLightingTechniqueCount; ++fi)
                {
                    buckets.push_back(MaterialResources::VariantBucketDesc{
                        .family       = static_cast<ELightingTechnique>(fi),
                        .feature_mask = 0u,
                    });
                }
            }
            return buckets;
        }

        /// Map a registered shading model to its builtin family fragment shader.
        /// Single source of truth shared by the Forward + Deferred init paths: the
        /// per-pass shader handles differ, but the family→frag switch was byte-
        /// identical in both. LegacyLit aliases Unlit until that family lands (S15);
        /// Graph returns @p fs_graph (null handle when no per-scene override is
        /// set, so that family's pipeline is skipped in registerFamilyPipelines).
        ///
        /// Why a handle rather than an object pointer: registerFamilyPipelines is
        /// the centralized application point for domain-merge switching
        /// (ShaderResources::mergedOrOriginal needs a handle); an object pointer
        /// here would only force the caller to dereference early and scatter the
        /// switching decision back out into each feature.
        static ShaderHandle resolveFragmentForFamily(
            EShadingModel sm,
            ShaderHandle  fs_unlit,
            ShaderHandle  fs_pbr,
            ShaderHandle  fs_stylized,
            ShaderHandle  fs_graph) noexcept
        {
            switch (sm)
            {
            case EShadingModel::UNLIT:                return fs_unlit;
            case EShadingModel::LEGACY_LIT_BASE:      return fs_unlit; // aliases Unlit (S15)
            case EShadingModel::PbrMetallicRoughness: return fs_pbr;
            case EShadingModel::STYLIZED:             return fs_stylized;
            case EShadingModel::GRAPH:                return fs_graph;
            default:                                  return ShaderHandle{};
            }
        }

        /// Build the per-family graphics pipelines (back-face-cull + a
        /// double-sided no-cull variant) for a GPU-driven mesh pass, filling
        /// `bucket_pipelines_` (the per-family BOOTSTRAP pipelines the
        /// BucketPipelinePool resolves a bucket to). Shared by Forward and
        /// Deferred — they differ only in @p base_template (blend/depth state),
        /// the vertex shader handle, and @p resolve_fragment (family → frag)。
        ///
        /// This is the centralized application point for domain-merge switching:
        /// every stage's handle is routed through ShaderResources::mergedOrOriginal
        /// inside this one method — the whole mesh family (every family variant of
        /// Forward / GBuffer / Highlight, plus the _vp shared by the material-graph
        /// bucket PSOs) switches in one place, so variants within the same pass are
        /// naturally switched together as a batch; there is no way to end up with
        /// "the main pipeline switched but a variant didn't," which would produce an
        /// incompatible layout. The vertex_shader field in the template is
        /// overwritten by this method — callers must not (and need not) fetch the
        /// module themselves.
        ///
        /// First shading model registered per family wins the family's slot;
        /// the _vp vertex shader reads vertices via set 7, so no vertex
        /// attributes are bound — only the vertex-pool layout spec is appended.
        [[nodiscard]] Expected<void> registerFamilyPipelines(
            const GraphicsPipelineTemplate&                    base_template,
            ShaderHandle                                       vertex_shader,
            VertexLayoutRegistry&                              vlr,
            VertexLayoutId                                     vp_read_layout,
            const std::function<ShaderHandle(EShadingModel)>&  resolve_fragment);

        /// For each Graph-family variant bucket carrying a baked per-material frag
        /// shader (R1), build (cached) that bucket's OWN graphics pipeline from the
        /// stored template/_vp/layout and register it in bucket_pipelines_, so
        /// pick() routes the bucket to its own PSO instead of the family bootstrap.
        /// @p use_gbuffer selects the gbuffer (deferred) vs forward shader. Each
        /// subclass calls this in addPasses (right after collectVariantBuckets).
        /// Buckets with no per-material shader stay on the bootstrap (unchanged).
        void registerGraphBucketPipelines(MaterialResources* mat_res, bool use_gbuffer);

        // =====================================================================
        //  Protected data — shared between forward and deferred
        // =====================================================================

        // --- Per-bucket draw pipeline pool. Holds the per-family BOOTSTRAP
        //     pipelines (back-face cull + a double-sided no-cull variant); the _vp
        //     vertex shader serves static + skinned draws. Built by
        //     registerFamilyPipelines(), read by the subclass's addPasses via
        //     `bucket_pipelines_.pick(bucket)` (one entry per variant bucket).
        //     R1 will give a bucket its own PSO here (see BucketPipelinePool). ---
        BucketPipelinePool bucket_pipelines_{};

        // registerFamilyPipelines 期存下的构建输入,好让 addPasses 用图材质运行期提交的
        // 片元着色器建它自己的管线(与家族 bootstrap 同一份模板 / _vp / 布局)。模板按
        // 指针持有,以免把它的完整定义拉进本头;缓存把图着色器句柄映到已建好的 PSO。
        std::unique_ptr<GraphicsPipelineTemplate>            graph_pso_template_;
        // The _vp ShaderInfo is held BY VALUE (not a ShaderObject*): the pointer
        // would dangle once a later compileShader grows + reallocates the
        // ShaderResources storage. The vp module lives in graph_pso_template_.
        lux::rdesc::ShaderInfo                               graph_pso_vp_info_;
        VertexLayoutRegistry*                                graph_pso_vlr_{ nullptr };
        VertexLayoutId                                       graph_pso_vp_layout_{};

        // Exact composite identity key for the per-material graph PSO cache: the
        // frag shader handle (index+gen) plus the double_sided render-state bit.
        // Using a struct with exact operator== — instead of XOR-folding the 65-bit
        // domain into one uint64 used directly as the map key — makes the identity
        // injective: distinct (shader, double_sided) combos can never collide onto
        // another material's PSO. The fold now lives only in the HASH, where
        // collisions are resolved by operator==. (C10)
        struct GraphPsoKey
        {
            uint32_t shader_index{};
            uint32_t shader_gen{};
            bool     double_sided{};
            bool operator==(const GraphPsoKey&) const noexcept = default;
        };
        struct GraphPsoKeyHash
        {
            std::size_t operator()(const GraphPsoKey& k) const noexcept
            {
                std::size_t h = std::hash<std::uint64_t>{}(
                    (static_cast<std::uint64_t>(k.shader_index) << 32) | k.shader_gen);
                h ^= std::hash<bool>{}(k.double_sided) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
                return h;
            }
        };
        std::unordered_map<GraphPsoKey, GraphicsPipelineHandle, GraphPsoKeyHash> graph_pso_cache_;

        // --- Three-stream instance storage (borrowed from SceneRegistry) ---
        InstanceResources *instance_res_{nullptr};

        // --- Cull resources ---
        VkDevice device_{VK_NULL_HANDLE};
        VkDescriptorSetLayout cull_set_layout_{VK_NULL_HANDLE};
        ComputePipelineHandle view_cull_pipeline_{};
        ComputePipelineHandle compact_pipeline_{};
        GpuDrivenMeshExtFlags extension_flags_{};
        uint32_t hzb_mode_spec_{0};

        // --- Render graph transient handles (set in addPasses, per-frame) ---
        RGResourceHandle draw_indirect_rg_{};
        RGResourceHandle draw_count_rg_{};
        RGResourceHandle visible_instance_rg_{};
        RGResourceHandle mdc_info_rg_{};  ///< Per-MDC GPU info buffer (binding 7)
        std::vector<RGResourceHandle> imported_index_buffers_rg_;

        VmaAllocator vma_{VK_NULL_HANDLE};

        // --- MDC graph recompile tracking ---
        // The MdcTable::layoutSerial() the current graph was compiled against. We
        // key recompiles on the layout serial (capacity-band crossings / bucket
        // birth-death) rather than mutationSerial (every instance add/remove), so
        // steady-state spawn/despawn within a bucket's power-of-two capacity band
        // reuses the compiled graph — no full recompile per frame. Per-MDC offsets
        // and transient buffer sizes derive from sticky capacity bands, so they are
        // unchanged whenever layoutSerial is. (P-7)
        uint64_t last_compiled_mdc_serial_{~0ull};
        uint64_t last_ibo_topology_serial_{~0ull};

        // Per-frame mesh instance extension data (replaces thread_local)
        MeshInstanceExtData frame_instance_ext_{};
    };
} // namespace lux::render
