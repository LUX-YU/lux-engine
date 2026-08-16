#pragma once

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <string>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC RenderClusterFeature final : public RenderFeature
    {
    public:
        struct Config final
        {
            std::string name{"RenderCluster"};
            ShaderHandle cluster_cull_compute_shader{};
            ShaderHandle candidate_expand_compute_shader{};
            ShaderHandle pick_vertex_shader{};
            ShaderHandle pick_fragment_shader{};
        };

        RenderClusterFeature();
        explicit RenderClusterFeature(Config config);

        Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        [[nodiscard]] bool canRebaseSceneOrigin(
            const std::int64_t origin_delta[3]) const noexcept override;
        void rebaseSceneOrigin(
            const std::int64_t origin_delta[3]) noexcept override;
        bool allocateViewState(
            std::uint32_t view,
            RenderScene& scene) override;
        void onFrameBegin(const FeatureFrameContext& context) override;
        void addPasses(RGBuilder& builder) override;

    private:
        Config config_{};
        ComputePipelineHandle cluster_cull_pipeline_{};
        ComputePipelineHandle candidate_expand_pipeline_{};
        GraphicsPipelineHandle pick_pipeline_{};
        VkDescriptorSetLayout cluster_cull_set_layout_{VK_NULL_HANDLE};
        VkDescriptorSetLayout candidate_expand_set_layout_{VK_NULL_HANDLE};
        VkDescriptorSetLayout pick_set_layout_{VK_NULL_HANDLE};
    };

    inline RenderClusterFeature::RenderClusterFeature()
        : RenderClusterFeature(Config{})
    {}
} // namespace lux::render
