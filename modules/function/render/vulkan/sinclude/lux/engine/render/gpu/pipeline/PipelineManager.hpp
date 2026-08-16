#pragma once
#include <lux/engine/render/core/Hash.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/render/gpu/pipeline/RenderPassKey.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/gpu/pipeline/ShaderPermutation.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <optional>
#include <vector>

#include <lux/cxx/container/SmallVector.hpp>

namespace lux::render { class DeviceContext; }
#include <string>

namespace lux::rdesc { struct ShaderInfo; }

namespace lux::render
{
    class GeneralDescriptorSetLayout; // Reflected layout: source of shared-domain (GLOBAL/BINDLESS) layouts
    class DescriptorService;          // Reflected layout: builds/deduplicates FEATURE-domain layouts
    class PipelineLayoutService;      // Reflected layout: deduplicates pipeline layouts

    struct PipelineManagerTelemetry final
    {
        std::uint64_t graphics_cache_hits{0u};
        std::uint64_t graphics_cache_misses{0u};
        std::uint64_t graphics_create_failures{0u};
        std::uint64_t graphics_create_nanoseconds{0u};
        std::uint64_t graphics_create_max_nanoseconds{0u};
        std::uint64_t compute_create_calls{0u};
        std::uint64_t compute_create_failures{0u};
        std::uint64_t compute_create_nanoseconds{0u};
        std::uint64_t compute_create_max_nanoseconds{0u};
        std::uint64_t render_pass_cache_hits{0u};
        std::uint64_t render_pass_cache_misses{0u};
    };

    /// One descriptor binding produced by reflection aggregation
    /// (per-binding detail is no longer discarded). `name` is the
    /// reconciliation key against LayoutContract — information reflection
    /// can't obtain, like binding flags, is looked up from the contract by
    /// name.
    struct ReflectedSetBinding {
        uint32_t           binding{0};
        VkDescriptorType   type{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
        uint32_t           count{1};
        VkShaderStageFlags stages{0};
        std::string        name{};
    };

    struct ReflectedSet {
        uint32_t set{0};
        lux::cxx::SmallVector<ReflectedSetBinding, 8> bindings;
    };

    /// The source classification of a pipeline-layout slot. See
    /// EPlannedSetKind in LayoutPlan.hpp — the two map one-to-one; this one
    /// answers "which category does a given slot of a given pipeline belong
    /// to," while that one answers "which sets exist across the whole
    /// graph."
    enum class ESlotSource : uint8_t
    {
        EngineShared,    ///< An engine-shared set; its shape comes from EngineSetShapes.
        FeatureExplicit, ///< A shared set a feature explicitly declares via explicit_set_layouts.
        PipelinePrivate, ///< Private to this pipeline; its shape is exactly its reflection.
        ReflectionHole,  ///< Not referenced by the shader; falls back to that slot's engine-shared layout.
        /// A merged, *multi-member* domain set (currently only the FEATURE
        /// domain: Instance/Light/Material/Particle/Compute/VertexPool
        /// sharing one set). logical_set holds the domain enum value
        /// (rdesc::EBindFrequency), not a canonical set number — several
        /// canonical sets live in this one slot, so a single number
        /// couldn't hold it; member identity is looked up from a reflected
        /// binding's name via the contract (both resource_slot_map
        /// synthesis and Plan aggregation do this). At record time this
        /// slot binds SceneDomainDescriptorSets' domain-set instance, never
        /// any single owner's per-set instance.
        ///
        /// Single-member domains (GLOBAL=Scene, BINDLESS=Texture) don't use
        /// this classification: their layout has the same shape before and
        /// after merging, the slot table is unchanged, and they keep using
        /// EngineShared — this has been confirmed by Skybox, and an
        /// already-verified path isn't touched purely for uniformity.
        DomainMerged,
    };

    /// The *explicit mapping* from slot -> logical set.
    ///
    /// Without it, "whose set does this slot actually hold" could only be
    /// reverse-inferred from three places — explicit_set_layouts,
    /// resource_slot_map, and reflection — and that inference goes wrong:
    /// macro overrides let the same logical set land at different slots in
    /// different pipelines (skinning's vertex pool sits at set1, but its
    /// canonical number is 7), and private sets with the same number in
    /// different pipelines have nothing to do with each other (Tonemap's
    /// set1 is the HDR input, Canvas2D's set1 is the instance buffer).
    /// Aggregating across the whole graph by slot number alone is bound to
    /// mismatch resources.
    struct ReflectedSlot
    {
        uint32_t              slot{0};        ///< Index within this pipeline's layout array.
        ESlotSource           source{ESlotSource::PipelinePrivate};
        /// Logical identity: for EngineShared/ReflectionHole, the engine's
        /// canonical set number; for everything else, the slot itself
        /// (needs the owning pipeline alongside it to be uniquely
        /// determined).
        uint32_t              logical_set{0};
        /// The layout decided at build time. FeatureExplicit uses it as an
        /// *identity key*: multiple pipelines sharing the same feature set
        /// are passed the same handle.
        VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    };

    /// A pass-private descriptor set resolved from the slot number used by
    /// the source shader to the slot used by the runtime pipeline layout.
    /// Domain merging may occupy the source slot and relocate the complete
    /// private set, so callers must use both values returned here together:
    /// `layout` for allocation and `runtime_slot` for binding.
    struct ResolvedPrivateSetLayout final
    {
        uint32_t source_slot{0u};
        uint32_t runtime_slot{0u};
        VkDescriptorSetLayout layout{VK_NULL_HANDLE};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return layout != VK_NULL_HANDLE;
        }
    };

    struct PipelineReflectedInfo {
        // Mask of active descriptor sets used by the pipeline (e.g., 0b101 means Set 0 and 2 are used).
        uint32_t active_sets_mask = 0;

        // Merged push constant ranges.
        lux::cxx::SmallVector<VkPushConstantRange, 2> push_constant_ranges;

        // Full per-set/binding reflection (the same set+binding is merged
        // across stages, with type/count consistency checked during the
        // merge). Feeds both reflected-layout building and LayoutPlan.
        lux::cxx::SmallVector<ReflectedSet, 4> reflected_sets;

        // Per-slot classification, aligned index-for-index with the
        // pipeline layout's set array (see ReflectedSlot).
        lux::cxx::SmallVector<ReflectedSlot, 8> slots;

        // Aggregated across shaders that took part in merging
        // (ShaderInfo::merged_domain_layout): true if any stage has already
        // been relocated under domain merging; layout routing uses this to
        // route engine resources to the domain layout.
        bool merged_domain_layout = false;
        // The flag disagrees between stages (e.g. VS merged, FS not) — the
        // two sides' engine-resource positions simply don't line up, so
        // this is an error at registration time rather than something left
        // to guess at record time.
        bool merged_flag_conflict = false;

        // The union of each stage's private-set relocation table
        // (ShaderInfo::private_set_remap): at record time, bare-slot
        // bindings (literal slot numbers like bindTransientDS(5, ...)) are
        // translated to their post-relocation slot through this. The same
        // `from` mapping to a different `to` in different stages is
        // treated as a conflict (sets merged_flag_conflict, errors at
        // registration time).
        lux::cxx::SmallVector<std::pair<uint32_t, uint32_t>, 4> private_set_remap;
    };

    inline VkSampleCountFlagBits to_vk_sample_count(uint32_t samples) noexcept
    {
        switch (samples)
        {
        case 1:  return VK_SAMPLE_COUNT_1_BIT;
        case 2:  return VK_SAMPLE_COUNT_2_BIT;
        case 4:  return VK_SAMPLE_COUNT_4_BIT;
        case 8:  return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        case 32: return VK_SAMPLE_COUNT_32_BIT;
        case 64: return VK_SAMPLE_COUNT_64_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
        }
    }

    // =============================
    // Hash and comparison for RenderPassKey
    // =============================

    struct RenderPassKeyHasher
    {
        std::size_t operator()(const RenderPassKey& key) const noexcept
        {
            std::size_t h = 0;
            hash_combine(h, static_cast<std::size_t>(key.samples));
            hash_combine(h, static_cast<std::size_t>(key.subpass_count));
            hash_combine(h, static_cast<std::size_t>(key.depth_stencil_format));

            for (uint32_t i = 0; i < key.color_count; ++i)
            {
                hash_combine(h, static_cast<std::size_t>(key.color_formats[i]));
            }
            return h;
        }
    };

    struct RenderPassKeyEqual
    {
        bool operator()(const RenderPassKey& a, const RenderPassKey& b) const noexcept
        {
            if (a.samples != b.samples)
                return false;
            if (a.subpass_count != b.subpass_count)
                return false;
            if (a.depth_stencil_format != b.depth_stencil_format)
                return false;
            if (a.color_count != b.color_count)
                return false;
            for (uint32_t i = 0; i < a.color_count; ++i)
            {
                if (a.color_formats[i] != b.color_formats[i])
                    return false;
            }
            return true;
        }
    };
    // Pipeline cache key
    // =============================
    struct PipelineKey
    {
        GraphicsPipelineHandle         template_handle;
        RenderPassKey                  render_pass_key;
        uint32_t                       subpass_index = 0;
        ShaderFeatureMask              features = 0;  // Shader permutation feature mask
        uint32_t                       specialization_hash = 0;

        /// Version number of the template's pipeline layout. Incremented
        /// whenever LayoutPlan rewrites the template's layout at
        /// graph-compile time — otherwise the same (template, render_pass,
        /// features) tuple would hit a cached PSO built against the old
        /// layout, silently using the wrong layout.
        uint32_t                       layout_epoch = 0;

        bool operator==(const PipelineKey& other) const noexcept
        {
            if (template_handle != other.template_handle)
                return false;
            if (subpass_index != other.subpass_index)
                return false;
            if (features != other.features)
                return false;
            if (specialization_hash != other.specialization_hash)
                return false;
            if (layout_epoch != other.layout_epoch)
                return false;

            RenderPassKeyEqual eq;
            return eq(render_pass_key, other.render_pass_key);
        }
    };

    struct PipelineKeyHasher
    {
        std::size_t operator()(const PipelineKey& key) const noexcept
        {
            std::size_t h = 0;
            hash_combine(h, static_cast<std::size_t>(key.template_handle.index));
            hash_combine(h, static_cast<std::size_t>(key.subpass_index));
            hash_combine(h, static_cast<std::size_t>(key.features));
            hash_combine(h, static_cast<std::size_t>(key.specialization_hash));
            hash_combine(h, static_cast<std::size_t>(key.layout_epoch));

            RenderPassKeyHasher rp_hasher;
            hash_combine(h, rp_hasher(key.render_pass_key));
            return h;
        }
    };

    /**
     * @brief Manages VkPipeline and VkRenderPass caching.
     *
     * @note Thread safety: This class is NOT thread-safe. All calls to
     *       registerGraphicsTemplate(), getOrCreatePipeline(), and
     *       getOrCreateRenderPass() must be serialized by the caller.
     *       Typically, pipeline creation happens on the main/render thread
     *       during frame setup, not from multiple threads concurrently.
     *       If multi-threaded pipeline creation becomes needed, add a
     *       std::shared_mutex (readers for cache hits, writer for misses).
     */
    class LUX_FUNCTION_PUBLIC PipelineManager
    {
    public:
        explicit PipelineManager(DeviceContext& device_ctx, bool use_dynamic_rendering = false);
        ~PipelineManager();

        /**
         * @brief Registers a graphics pipeline template and returns a handle.
         *
         * @param description The pipeline template description.
         * @param shader_infos Optional shader reflection info for set/push-constant metadata.
         * @return GraphicsPipelineHandle The handle to the registered template.
         *
         * @note Two layout paths exist here, during the migration:
         *   - description.pipeline_layout is non-null -> the legacy path,
         *     used as-is (a hand-assembled layout);
         *   - it's null and shader_infos were supplied + setReflectedLayoutEnv
         *     has been injected -> the *reflected layout* path: each set is
         *     routed by the frequency domain from LayoutContract —
         *     GLOBAL/BINDLESS take the engine-shared layout, the rest are
         *     built from reflection + the contract's binding_flags via
         *     DescriptorService; the pipeline layout itself is produced,
         *     deduplicated, via PipelineLayoutService. The resulting layouts
         *     can be queried via templateSetLayout() (used when a feature
         *     allocates a private set instance).
         */
        Expected<GraphicsPipelineHandle> registerGraphicsTemplate(
            const GraphicsPipelineTemplate& description,
            std::span<const rdesc::ShaderInfo* const> shader_infos = {}
        );

        /// Injects the three services needed to build the reflected layout
        /// (called once when RenderContext is constructed).
        void setReflectedLayoutEnv(GeneralDescriptorSetLayout& shared,
                                   DescriptorService& descriptors,
                                   PipelineLayoutService& pipeline_layouts) noexcept;

        /// Under the reflected-layout path, the VkDescriptorSetLayout for a
        /// given set slot of the template (used when a feature allocates a
        /// private set instance). Returns VK_NULL_HANDLE for a legacy
        /// (hand-assembled) template or an out-of-range slot.
        [[nodiscard]] VkDescriptorSetLayout templateSetLayout(
            GraphicsPipelineHandle handle, uint32_t set) const noexcept;

        /// Resolves a source-shader private set through domain relocation.
        /// Returns an invalid result if the template is legacy, the source
        /// set is absent, or the resolved slot is not pipeline-private.
        [[nodiscard]] ResolvedPrivateSetLayout templatePrivateSetLayout(
            GraphicsPipelineHandle handle,
            uint32_t source_set) const noexcept;

        /// Registers a compute pipeline's reflected layout: the layout is
        /// built from a single shader's reflection + the contract (the same
        /// routing rules as graphics), then goes through the existing
        /// creation path. The set layout can be queried via
        /// computeSetLayout() (used when a feature allocates a pass-local
        /// set instance). explicit_set_layouts has the same meaning as the
        /// field of the same name on GraphicsPipelineTemplate (a set a
        /// feature owns, shared across pipelines, of which each pipeline
        /// only uses a subset).
        Expected<ComputePipelineHandle> registerComputePipelineReflected(
            VkShaderModule shader,
            const rdesc::ShaderInfo& info,
            std::string debug_name,
            std::span<const GraphicsPipelineTemplate::ShaderSpecializationValue> specialization_values = {},
            std::span<const std::pair<uint32_t, VkDescriptorSetLayout>> explicit_set_layouts = {});

        [[nodiscard]] VkDescriptorSetLayout computeSetLayout(
            ComputePipelineHandle handle, uint32_t set) const noexcept;

        /// Rewrites a template's pipeline layout and set-layout table at
        /// graph-compile time. Once LayoutAllocator produces a LayoutPlan,
        /// compile calls this method on the affected templates **before**
        /// buildCompiledPasses (after that point the layout gets
        /// snapshotted into the compiled pass and used to build the PSO).
        /// Internally increments the template's layout_epoch, so PSOs
        /// cached against the old layout no longer get hit (see
        /// PipelineKey::layout_epoch).
        ///
        /// Timing safety: all template registration happens before compile
        /// (inside feature attach and addPasses), and pipeline_templates_
        /// doesn't grow during compile, so this in-place rewrite doesn't
        /// invalidate any existing references.
        Expected<void> finalizeTemplateLayout(
            GraphicsPipelineHandle handle,
            VkPipelineLayout layout,
            std::span<const VkDescriptorSetLayout> set_layouts);

        /// The template's current layout version number (part of the PSO
        /// key).
        [[nodiscard]] uint32_t templateLayoutEpoch(GraphicsPipelineHandle handle) const noexcept;

        /// The template's full per-binding reflection (input to
        /// LayoutAllocator). RenderGraphCompiler retrieves it per pass ->
        /// template handle during compile, aggregates the whole graph's
        /// resource requirements, and allocates all sets/bindings in one
        /// pass. Returns nullptr when there is no reflection data (a
        /// legacy, hand-assembled template).
        /// Rebuilds this template's pipeline layout from the given array of
        /// sets and writes it back (deduplicated via PipelineLayoutService —
        /// an identical array reuses the same object). This is the entry
        /// point LayoutPlan writes back through — the graph-compile-time
        /// allocator decides the positions; this function is only
        /// responsible for landing the result on the template, while
        /// creating and deduplicating the Vulkan objects stays inside this
        /// class.
        ///
        /// Does **nothing and returns false** when the array is already
        /// identical to the current one. This matters: unconditionally
        /// incrementing `layout_epoch` on every graph recompile would
        /// invalidate every cached PSO (PipelineKey includes the epoch),
        /// triggering an avalanche of rebuilds.
        Expected<bool> rebuildTemplateLayout(
            GraphicsPipelineHandle handle,
            std::span<const VkDescriptorSetLayout> set_layouts);

        /// The device's maxBoundDescriptorSets (0 = the reflected-layout
        /// environment hasn't been injected).
        [[nodiscard]] uint32_t maxBoundDescriptorSets() const noexcept;

        [[nodiscard]] const PipelineReflectedInfo* templateReflection(
            GraphicsPipelineHandle handle) const noexcept;

        /// The equivalent reflection data for compute pipelines (indices
        /// aligned with compute_pipelines_).
        [[nodiscard]] const PipelineReflectedInfo* computeReflection(
            ComputePipelineHandle handle) const noexcept;

        /**
         * @brief Retrieves or creates a VkPipeline based on the template, render pass key, and subpass index.
         *
         * @param template_handle The handle of the pipeline template.
         * @param render_pass_key The key describing the render pass compatibility.
         * @param subpass_index The subpass index within the render pass.
         * @return VkPipeline The requested pipeline.
         */
        VkPipeline getOrCreatePipeline(GraphicsPipelineHandle template_handle, const RenderPassKey& render_pass_key, uint32_t subpass_index);

        /**
         * @brief Retrieves or creates a VkPipeline with shader permutation features.
         *
         * When features != 0, specialization constants are injected into shader stages
         * based on the feature mask. This allows a single SPIR-V module to produce
         * multiple pipeline variants without recompilation.
         */
        VkPipeline getOrCreatePipeline(GraphicsPipelineHandle template_handle, const RenderPassKey& render_pass_key, uint32_t subpass_index, ShaderFeatureMask features);

        /**
         * @brief Retrieves or creates a VkRenderPass based on the key.
         *
         * @param key The render pass key.
         * @return VkRenderPass The requested render pass.
         */
        VkRenderPass getOrCreateRenderPass(const RenderPassKey& key);
        const GraphicsPipelineTemplate& getTemplate(GraphicsPipelineHandle handle) const;


        // -----------------------------------------------------------------
        // Compute pipeline management
        // -----------------------------------------------------------------

        /**
         * @brief Register a compute pipeline.
         *
         * @param shader  Pre-created VkShaderModule for the compute stage.
         * @param layout  Pre-created VkPipelineLayout.
         * @return ComputePipelineHandle  Stable handle for later retrieval.
         *
         * @note Ownership of @p shader is NOT transferred; the caller is
         *       responsible for destroying it after registration.
         *       Ownership of @p layout is NOT transferred.
         */
        [[nodiscard]] ComputePipelineHandle registerComputePipeline(
            VkShaderModule shader,
            VkPipelineLayout layout,
            std::span<const GraphicsPipelineTemplate::ShaderSpecializationValue> specialization_values = {});

        [[nodiscard]] VkPipeline      getComputePipeline(ComputePipelineHandle handle) const noexcept;
        [[nodiscard]] VkPipelineLayout getComputeLayout (ComputePipelineHandle handle) const noexcept;

        /// @brief Return count of registered templates
        [[nodiscard]] uint32_t templateCount() const noexcept { return static_cast<uint32_t>(pipeline_templates_.size()); }

        /**
         * @brief Explicitly destroys all managed resources.
         */
        void destroyAll();

        // (setPermutationCompiler/permutationCompiler removed: variants now
        //  only use specialization constants, and ShaderPermutationCompiler
        //  has been slimmed down to a pure static utility, so instance
        //  injection is no longer needed.)

        /// @brief Query whether this manager creates pipelines for dynamic rendering
        [[nodiscard]] bool useDynamicRendering() const noexcept { return use_dynamic_rendering_; }

        [[nodiscard]] const PipelineManagerTelemetry& telemetry() const
            noexcept
        {
            return telemetry_;
        }

        [[nodiscard]] static constexpr uint32_t variantBudgetPerTemplate() noexcept { return kVariantBudgetPerTemplate; }

        /// 自发上报的去处(非拥有,由 RenderContext 在构造后装上)。管线创建失败与
        /// 变体预算耗尽都发生在图编译期,调用方按 VK_NULL_HANDLE 当作"这次没准备好"
        /// 并在下次重编时重试 —— 没有谁能就地处置,但也不能不可见。
        void setErrorSink(RenderErrorSink* sink) noexcept { error_sink_ = sink; }

    private:
        static constexpr uint32_t kVariantBudgetPerTemplate = 256;
        RenderErrorSink* error_sink_ = nullptr;
        DeviceContext* device_ctx_ = nullptr;
        bool use_dynamic_rendering_ = false;  ///< When true, pipelines use VkPipelineRenderingCreateInfo instead of VkRenderPass
        PipelineManagerTelemetry telemetry_{};
        std::vector<GraphicsPipelineTemplate> pipeline_templates_;
        std::vector<std::vector<ShaderFeatureMask>> template_variant_masks_;
        std::vector<uint64_t> template_variant_fallback_counts_;
        std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKeyHasher, RenderPassKeyEqual> render_pass_cache_;

        struct PipelineRecord
        {
            VkPipeline                      pipeline        = VK_NULL_HANDLE;
            VkRenderPass                    render_pass     = VK_NULL_HANDLE;
            GraphicsPipelineHandle          template_handle;
            RenderPassKey                   render_pass_key{};
            uint32_t                        subpass_index = 0;
        };

        std::unordered_map<PipelineKey, PipelineRecord, PipelineKeyHasher> pipeline_cache_;

        // Compute pipeline storage
        struct ComputePipelineRecord
        {
            VkPipeline       pipeline = VK_NULL_HANDLE;
            VkPipelineLayout layout   = VK_NULL_HANDLE;
        };
        std::vector<ComputePipelineRecord> compute_pipelines_;

    private:
        Expected<VkRenderPass> create_render_pass_internal(const RenderPassKey& key);
        Expected<VkPipeline>   create_pipeline_internal(const GraphicsPipelineTemplate& tmpl, VkRenderPass render_pass, uint32_t subpass_index, const RenderPassKey& render_pass_key, ShaderFeatureMask features = 0);

        /// Builds the reflected layout: routes each set's layout by
        /// ownership, producing the pipeline layout and the per-set layout
        /// table; a reflection hole is filled with the shared layout
        /// according to the caller's resource_slot_map slot semantics (to
        /// keep record-time binding compatible).
        /// Also writes the per-slot classification into `reflected.slots`
        /// (input to the allocator) — the classification is *known* right
        /// here, and letting a downstream consumer re-derive it would only
        /// get it wrong, so it's recorded in place instead.
        Expected<VkPipelineLayout> buildReflectedPipelineLayout(
            PipelineReflectedInfo& reflected,
            const GraphicsPipelineTemplate& description,
            lux::cxx::SmallVector<VkDescriptorSetLayout, 8>& out_set_layouts);

        // The reflected-layout environment (injected by
        // setReflectedLayoutEnv; the legacy path doesn't depend on it).
        GeneralDescriptorSetLayout* shared_layouts_{nullptr};
        DescriptorService*          descriptor_service_{nullptr};
        PipelineLayoutService*      pipeline_layout_service_{nullptr};

        /// Per-template set-layout table built from reflection (empty for
        /// legacy templates); indices aligned with pipeline_templates_. The
        /// data source for templateSetLayout().
        std::vector<lux::cxx::SmallVector<VkDescriptorSetLayout, 8>> template_set_layouts_;

        /// The equivalent table for compute pipelines (indices aligned with
        /// compute_pipelines_; empty for legacy pipelines).
        std::vector<lux::cxx::SmallVector<VkDescriptorSetLayout, 8>> compute_set_layouts_;

        /// Full reflection kept per template / per compute pipeline (input
        /// to the allocator). has_value()==false means that pipeline's
        /// registration didn't supply shader_infos (the legacy path).
        std::vector<std::optional<PipelineReflectedInfo>> template_reflections_;
        std::vector<std::optional<PipelineReflectedInfo>> compute_reflections_;

        /// Per-template layout version number (incremented by
        /// finalizeTemplateLayout; part of PipelineKey).
        std::vector<uint32_t> template_layout_epochs_;
    };
} // namespace lux::render
