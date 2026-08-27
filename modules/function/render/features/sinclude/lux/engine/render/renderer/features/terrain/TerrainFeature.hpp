#pragma once

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/renderer/features/terrain/TerrainResources.hpp>
#include <lux/engine/function/render/client/core/PipelineHandle.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lux::render
{
    class ViewCameraResource;

    class LUX_FUNCTION_PUBLIC TerrainFeature final : public RenderFeature
    {
    public:
        struct Config final
        {
            std::string name{"Terrain"};
            std::uint32_t page_capacity{128u};
            /// Hard per-view GPU work budget. Each selected patch expands to
            /// 6 * 34 * 34 vertices, so an unbounded page_capacity * 64 limit
            /// can turn a transient selection anomaly into a Windows TDR.
            std::uint32_t maximum_selected_patches{1024u};
            float wanted_radius{2048.0f};
            std::uint64_t demotion_delay_frames{120u};
            ShaderHandle patch_select_shader{};
            ShaderHandle patch_vertex_shader{};
            ShaderHandle patch_fragment_shader{};
        };

        TerrainFeature();
        explicit TerrainFeature(Config config);

        Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        [[nodiscard]] bool canRebaseSceneOrigin(const std::int64_t origin_delta[3]) const noexcept override;
        void rebaseSceneOrigin(const std::int64_t origin_delta[3]) noexcept override;
        void onFrameBegin(const FeatureFrameContext& context) override;
        void addPasses(RGBuilder& builder) override;

    private:
        Config config_{};
        TerrainResources* resources_{nullptr};
        ViewCameraResource* cameras_{nullptr};
        std::vector<TerrainResources::ViewOrigin> wanted_views_;
        ComputePipelineHandle patch_select_pipeline_{};
        GraphicsPipelineHandle patch_pipeline_{};
        VkDescriptorSetLayout patch_select_layout_{VK_NULL_HANDLE};
        VkDescriptorSetLayout patch_layout_{VK_NULL_HANDLE};
        std::uint32_t patch_slot_{2u};
    };

    inline TerrainFeature::TerrainFeature() : TerrainFeature(Config{})
    {
    }
} // namespace lux::render
