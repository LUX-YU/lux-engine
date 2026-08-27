#pragma once
// ============================================================================
//  LinearDepthFeature — SceneDepth → 线性深度(LinearDepth 槽)的生产者
//
//  收官件:TargetSlot::LINEAR_DEPTH 注释多年前点名的用途
//  (sensors / SSAO / DoF)第一次有了生产者,同时是槽声明管道的首个
//  真实用户:requiredTargetSlots() 一行声明"补 LinearDepth + 读 SceneDepth",
//  物理背衬 / usage 追加 / 图导入全部由引擎自动补齐。
// ============================================================================
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/render/client/core/PipelineHandle.hpp>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp> // TargetSlot
#include <lux/engine/function/render/client/genops/LinearDepthOperation.ops.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

namespace lux::render
{

    class LUX_FUNCTION_PUBLIC LinearDepthFeature : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
        };

        explicit LinearDepthFeature(Config cfg) : cfg_(cfg)
        {
        }

        std::string_view name() const override
        {
            return "LinearDepth";
        }

        /// 补 LinearDepth(缺槽,按形状表 R32F 建)+ 读 SceneDepth(既有槽,
        /// 自动追加 SAMPLED)—— 槽声明管道的两种语义各用一位。
        uint32_t requiredTargetSlots() const override
        {
            return (1u << static_cast<uint32_t>(TargetSlot::LINEAR_DEPTH)) |
                   (1u << static_cast<uint32_t>(TargetSlot::SCENE_DEPTH));
        }

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;

    private:
        Config cfg_;
        GraphicsPipelineHandle pipeline_{kInvalidPipelineHandle};
        VkDescriptorSetLayout depth_ds_layout_{VK_NULL_HANDLE}; ///< set1:深度采样(反射布局)
        VkSampler depth_sampler_{VK_NULL_HANDLE};               ///< 服务缓存句柄(最近邻:深度不得插值)
    };

} // namespace lux::render
