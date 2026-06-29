#pragma once
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/resources/lifecycle/FifOwned.hpp>
#include <lux/engine/render/renderer/features/postprocess/TonemapParams.hpp>
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

enum class EToneMapOperator : uint8_t
{
    REINHARD     = 0,
    ACES_FILMIC  = 1,
    UNCHARTED2   = 2,
    NONE         = 3    ///< pass-through (debug: view raw HDR values)
};

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
        ShaderHandle           vertex_shader{};     ///< fullscreen triangle
        ShaderHandle           fragment_shader{};   ///< tonemap.frag
        EToneMapOperator   tone_map_op = EToneMapOperator::ACES_FILMIC;
        float              exposure    = 1.0f;
        float              gamma       = 2.2f;
        std::string        color_input{"LitColor"};   ///< Name of the lit color source to read
        std::string        color_target{"SceneColor"};  ///< Name of the output color target to write
    };

    explicit TonemapFeature(Config cfg);
    ~TonemapFeature() override;

    std::string_view name() const override { return "Tonemap"; }

    void initAndAttachTo(RenderScene& scene) override;
    void onDetachFromScene(RenderScene& scene) override;
    void addPasses(RGBuilder& builder) override;
    void onFrameBegin(const FeatureFrameContext& ctx) override;

    // Runtime mutators write the LIVE param struct (read each frame by the
    // tonemap push constant). cfg_ stays the bring-up SSOT (shaders + targets).
    void setExposure(float exposure)             { params_.exposure = exposure; }
    void setGamma(float gamma)                   { params_.gamma    = gamma; }
    void setToneMapOperator(EToneMapOperator op) { params_.tone_map_op = static_cast<std::uint32_t>(op); }

    // --- Feature-driven quality seam (first adopter; all params are HOT) ---
    [[nodiscard]] std::string_view paramStructName() const override
    {
        return "lux::render::TonemapParams";
    }
    [[nodiscard]] void* paramData() noexcept override { return &params_; }
    [[nodiscard]] std::size_t paramSize() const noexcept override { return sizeof(TonemapParams); }
    EParamApply applyParams(const void* src, std::size_t size) override
    {
        if (src == nullptr || size != sizeof(TonemapParams))
            return EParamApply::UNSUPPORTED;
        TonemapParams next{};
        std::memcpy(&next, src, sizeof(TonemapParams));
        pending_params_ = next;   // applied at the next onFrameBegin (per-frame hook)
        return EParamApply::HOT;  // exposure/gamma/op are push constants — no rebuild
    }

private:
    void init();
    void destroy() noexcept;

    Config                       cfg_;
    TonemapParams                params_{};         ///< live, render-thread-owned tunable params
    std::optional<TonemapParams> pending_params_{}; ///< staged by applyParams; applied in onFrameBegin
    GraphicsPipelineHandle  tonemap_pipeline_{kInvalidPipelineHandle};

    // HDR input descriptor set layout (1× combined_image_sampler) — used by transient DS
    VkDescriptorSetLayout   hdr_ds_layout_{VK_NULL_HANDLE};
    // FifOwned: the sampler is retired through the FIF deferred-destroy queue on
    // destruction — no inline vkDestroySampler, no onDetach discipline. (C1)
    FifOwned<VkSampler>     hdr_sampler_;
};

} // namespace lux::render
