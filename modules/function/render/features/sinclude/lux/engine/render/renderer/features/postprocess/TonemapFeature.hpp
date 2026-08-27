#pragma once
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/gpu/lifecycle/FifOwned.hpp>
#include <lux/engine/function/render/client/features/postprocess/TonemapOperation.hpp>
#include <lux/engine/function/render/client/features/postprocess/TonemapParams.hpp>
#include <lux/engine/render/renderer/features/postprocess/TonemapPassParams.hpp> // PassParams 作者头
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <cstdint>

namespace lux::render
{

    /// 真身在 comm 层 TonemapOperation.hpp(客户端用枚举名配置);别名保持
    /// server 侧既有引用不变。
    using EToneMapOperator = ETonemapOperator;

    /**
     * @brief HDR → SDR tone mapping pass (fullscreen triangle).
     *
     * Reads the HDR color target published by DeferredLightingFeature and
     * writes the tone-mapped result to the swapchain backbuffer.
     *
     * Push constants carry exposure, gamma and operator selection so
     * they can be changed at runtime without pipeline recreation.
     */
    class LUX_FUNCTION_PUBLIC TonemapFeature : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle vertex_shader{};   ///< fullscreen triangle
            ShaderHandle fragment_shader{}; ///< tonemap.frag
            EToneMapOperator tone_map_op = EToneMapOperator::ACES_FILMIC;
            float exposure = 1.0f;
            float gamma = 2.2f;
            std::string color_input{"WaterColor"};  ///< Environment + water HDR source
            std::string color_target{"SceneColor"}; ///< Name of the output color target to write
        };

        explicit TonemapFeature(Config cfg);
        ~TonemapFeature() override;

        std::string_view name() const override
        {
            return "Tonemap";
        }

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void onDetachFromScene(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;
        void onFrameBegin(const FeatureFrameContext& ctx) override;

        // Runtime mutators write the LIVE param struct (read each frame by the
        // tonemap push constant). cfg_ stays the bring-up SSOT (shaders + targets).
        void setExposure(float exposure)
        {
            pass_params_.scalars.exposure = exposure;
        }
        void setGamma(float gamma)
        {
            pass_params_.scalars.gamma = gamma;
        }
        void setToneMapOperator(EToneMapOperator op)
        {
            pass_params_.scalars.tone_map_op = static_cast<std::uint32_t>(op);
        }

        // --- Feature-driven quality seam (first adopter; all params are HOT) ---
        // 编辑器面板投影 = PassParams 的标量段。paramStructName 仍指向
        // TonemapParams(反射类型不变,面板与 comm 载荷零感知)。
        [[nodiscard]] std::string_view paramStructName() const override
        {
            return "lux::render::TonemapParams";
        }
        [[nodiscard]] void* paramData() noexcept override
        {
            return &pass_params_.scalars;
        }
        [[nodiscard]] std::size_t paramSize() const noexcept override
        {
            return sizeof(TonemapParams);
        }
        EParamApply applyParams(const void* src, std::size_t size) override
        {
            if (src == nullptr || size != sizeof(TonemapParams))
                return EParamApply::UNSUPPORTED;
            TonemapParams next{};
            std::memcpy(&next, src, sizeof(TonemapParams));
            pending_params_ = next;  // applied at the next onFrameBegin (per-frame hook)
            return EParamApply::HOT; // exposure/gamma/op are push constants — no rebuild
        }

    private:
        [[nodiscard]] lux::render::Expected<void> init();
        void destroy() noexcept;

        Config cfg_;
        /// PassParams 活实例(渲染线程持有)。资源段在 addPasses 填充
        ///(图编译时),标量段被 recorder 每帧整体 memcpy 进推送常量 —— 同一份
        /// 结构同时是图声明的来源、PC 的来源、编辑器面板的来源。
        TonemapPassParams pass_params_{};
        std::optional<TonemapParams> pending_params_{}; ///< staged by applyParams; applied in onFrameBegin
        GraphicsPipelineHandle tonemap_pipeline_{kInvalidPipelineHandle};

        // HDR input descriptor set layout (1× combined_image_sampler) — used by transient DS
        VkDescriptorSetLayout hdr_ds_layout_{VK_NULL_HANDLE};
        /// DescriptorService 采样器缓存的共享句柄 —— 服务持有生命周期。
        VkSampler hdr_sampler_{VK_NULL_HANDLE};
    };

} // namespace lux::render
