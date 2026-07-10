#pragma once
/**
 * @file SkinningFeature.hpp
 * @brief Render-graph COMPUTE feature that pre-skins vertices.
 *
 * Stage R5.3 of the render-refactor.
 *
 * Owns the skinning compute pipeline + its 3-SSBO descriptor set, and the
 * per-scene SkinningResources (bone palette + transient skinned-vertex
 * output pool, registered into the scene's VertexPoolRegistry).
 *
 * Each frame addPasses() emits ONE compute pass that iterates
 * SkinningResources::dispatches() (built during command processing from
 * the UploadBonePalette command, R5.5) and dispatches skin_compute.comp
 * once per skinned mesh instance. The output pool is imported as an RG
 * resource and declared written, so when the mesh draw passes declare a
 * read on it (R5.4) the graph inserts the compute→vertex barrier
 * automatically.
 *
 * The descriptor set binds three buffers directly (not via the bindless
 * array): binding 0 = global VBO (input, un-skinned, with bone data),
 * binding 1 = bone palette, binding 2 = transient output pool. The
 * bindless set 7 is only for the graphics mesh shaders that consume the
 * OUTPUT.
 */

#include <array>
#include <cstdint>

#include <vulkan/vulkan.h>

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>          // ShaderHandle
#include <lux/engine/render/core/RenderTypes.hpp>             // kMaxFramesInFlight
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp> // ComputePipelineHandle
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>   // DescriptorLayoutId
#include <lux/engine/render/graph/RGPassTypes.hpp>             // RGResourceHandle
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    class SkinningResources;

    class LUX_FUNCTION_PUBLIC SkinningFeature : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle  compute_shader{};        ///< skin_compute.comp
            std::uint32_t max_bones{64u * 1024};   ///< bone-palette capacity
            VkDeviceSize  output_pool_bytes{16ull * 1024 * 1024};
        };

        SkinningFeature();
        explicit SkinningFeature(Config cfg);

        std::string_view name() const override { return "Skinning"; }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;

    private:
        void init(const Config& cfg);
        [[nodiscard]] SkinningResources* skinningResources() noexcept;

        /// DSResolverFn (RGPassTypes) for bindResourceDS: returns the skin DS for
        /// the current ring slot (SkinningResources::currentFrameIndex()), so the
        /// bound set's palette binding matches the slot the upload command wrote
        /// this frame. @p frame_slot (framework FIF index) is only a fallback.
        static VkDescriptorSet resolveSkinDS(const void* self, std::uint32_t frame_slot) noexcept;

        Config cfg_{};

        ComputePipelineHandle compute_pipeline_{};
        VkPipelineLayout      compute_layout_{VK_NULL_HANDLE};

        DescriptorLayoutId    skin_ds_layout_id_{kInvalidDescriptorLayoutId};
        // Per-FIF skin descriptor sets: identical except binding 1 (bone palette)
        // points at that ring slot's palette buffer (S0.2 / H4).
        std::array<VkDescriptorSet, kMaxFramesInFlight> skin_ds_{};
        SkinningResources*    skin_res_{nullptr};   ///< cached for resolveSkinDS

        RGResourceHandle      out_pool_rg_{};
    };

    // No-arg ctor defined out-of-class so Config{} is evaluated where the class is
    // complete (GCC 11/12 reject Config{} / {} as an in-class default argument).
    inline SkinningFeature::SkinningFeature() : SkinningFeature(Config{}) {}

} // namespace lux::render
