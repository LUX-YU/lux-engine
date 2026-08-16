#pragma once
// ============================================================================
//  SsaoFeature — 屏幕空间环境光遮蔽(着色输入槽的树内参考实现)
//
//  真正要打样的不是 AO 算法(见 ssao.frag.lglsl 头注),而是**旁路消费
//  的完整通路**:图内产出 → Light b11 描述符 → lit 路径无条件采样。
//
//  两个 pass 的分工:
//    SsaoResolve  读 LinearDepth 槽、写自有 persistent AO 纹理(全屏三角)
//    SsaoPublish  read(AO, SAMPLED) + markSideEffect —— 它不录任何 GPU
//                 命令,存在意义有二:①让图把 AO 转到 SHADER_READ_ONLY
//                 并排好 barrier(b11 的消费者在图外,图看不见那条读边);
//                 ②record 期解析物理 view,变化时经
//                 LightResources::provideShadingInput 重指 b11。
//
//  v0 边界:AO 纹理与 b11 槽都是场景粒度 ——
//  多视口场景各 view 重画同一张 persistent(后画者胜)。逐视图正确性
//  随「逐视图 ViewContext / 资源版本化」挂账项一起来。
// ============================================================================
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/core/PipelineHandle.hpp>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>   // TargetSlot
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>
#include <lux/engine/function/render/client/genops/SsaoOperation.ops.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

namespace lux::render
{

class LightResources;

class LUX_FUNCTION_PUBLIC SsaoFeature : public RenderFeature
{
public:
    /// 空 tag 生成约定:createFn 用 `SsaoFeature::Config{}` 构造特性,
    /// 无参数也要有这个名字(Hzb 同款)。
    struct Config
    {
    };

    SsaoFeature() = default;
    explicit SsaoFeature(Config) {}

    std::string_view name() const override { return "Ssao"; }

    /// 声明读 LinearDepth 槽(生产者是 LinearDepthFeature;本特性只添
    /// SAMPLED 用途)。SceneDepth 不需要 —— 线性化已由生产者做完。
    uint32_t requiredTargetSlots() const override
    {
        return 1u << static_cast<uint32_t>(TargetSlot::LINEAR_DEPTH);
    }

    lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
    void onDetachFromScene(RenderScene& scene) override;
    void addPasses(RGBuilder& builder) override;

private:
    GraphicsPipelineHandle pipeline_{kInvalidPipelineHandle};
    VkDescriptorSetLayout  input_ds_layout_{VK_NULL_HANDLE};  ///< set1:uLinearDepth(反射布局)
    VkSampler              input_sampler_{VK_NULL_HANDLE};    ///< 服务缓存句柄
    LightResources*        light_res_{nullptr};               ///< b11 的接收端(场景注册表)
    VkImageView            provided_view_{VK_NULL_HANDLE};    ///< 上次交给 b11 的 view(变化检测)
};

} // namespace lux::render
