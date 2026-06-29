#pragma once
#include <lux/engine/render/core/Hash.hpp>
#include <lux/engine/render/core/Errors.hpp>
#include <lux/engine/render/pipeline/RenderPassKey.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/pipeline/ShaderPermutation.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <lux/cxx/container/SmallVector.hpp>

namespace lux::render { class DeviceContext; }
#include <string>

namespace lux::rdesc { struct ShaderInfo; }

namespace lux::render
{
    class ShaderPermutationCompiler;  // Forward declaration (only pointer used)

    struct PipelineReflectedInfo {
        // Mask of active descriptor sets used by the pipeline (e.g., 0b101 means Set 0 and 2 are used).
        uint32_t active_sets_mask = 0;
        
        // Merged push constant ranges.
        lux::cxx::SmallVector<VkPushConstantRange, 2> push_constant_ranges;
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
         * @note description.pipeline_layout must be a valid layout created by
         *       PipelineLayoutService. PipelineManager does not create or own
         *       pipeline layouts.
         */
        Expected<GraphicsPipelineHandle> registerGraphicsTemplate(
            const GraphicsPipelineTemplate& description,
            const std::vector<const rdesc::ShaderInfo*>& shader_infos = {}
        );

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

        /**
         * @brief Find the first registered template matching a given geometry type.
         *
         * Useful when a pass processor needs to switch pipelines based on the
         * current RenderItem's geometry_type.  Returns kInvalidPipelineHandle
         * if no template with the requested type has been registered.
         */
        [[nodiscard]] GraphicsPipelineHandle findTemplateForGeometry(EGeometryType type) const noexcept;

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

        /// @brief Attach an external ShaderPermutationCompiler for feature-based variants
        void setPermutationCompiler(ShaderPermutationCompiler* compiler) { permutation_compiler_ = compiler; }
        ShaderPermutationCompiler* permutationCompiler() const { return permutation_compiler_; }

        /// @brief Query whether this manager creates pipelines for dynamic rendering
        [[nodiscard]] bool useDynamicRendering() const noexcept { return use_dynamic_rendering_; }

        struct VariantBudgetStats
        {
            uint32_t unique_feature_masks{0};
            uint64_t fallback_count{0};
        };
        [[nodiscard]] VariantBudgetStats variantBudgetStats(GraphicsPipelineHandle handle) const noexcept;
        [[nodiscard]] static constexpr uint32_t variantBudgetPerTemplate() noexcept { return kVariantBudgetPerTemplate; }
    private:
        static constexpr uint32_t kVariantBudgetPerTemplate = 256;
        DeviceContext* device_ctx_ = nullptr;
        ShaderPermutationCompiler* permutation_compiler_ = nullptr;  ///< Optional; set via setPermutationCompiler()
        bool use_dynamic_rendering_ = false;  ///< When true, pipelines use VkPipelineRenderingCreateInfo instead of VkRenderPass
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
    };
} // namespace lux::render
