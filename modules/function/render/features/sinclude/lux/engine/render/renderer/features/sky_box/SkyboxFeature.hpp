#pragma once
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/features/sky_box/SkyboxOperation.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
// RTextureHandle(此前由 SkyboxSyncCommands.hpp 间接带入)
#include <cstdint>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>

#include <array>
#include <optional>
#include <string>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC SkyboxFeature : public RenderFeature
    {
    public:
        struct Config
        {
            ShaderHandle vertex_shader{};
            ShaderHandle cubemap_fragment{};
            ShaderHandle equirect_fragment{};
            std::string color_input{"LitColor"};
            std::string depth_target{"SceneDepth"};
        };

        explicit SkyboxFeature(Config cfg);
        ~SkyboxFeature() override;

        std::string_view name() const override
        {
            return "Skybox";
        }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;

        // ---- RenderFeature lifecycle (render thread) ----
        void addPasses(RGBuilder& builder) override;

        bool isLoaded() const noexcept
        {
            return active_mode_ != ActiveMode::NONE;
        }

    private:
        /// 装载期建管线。返回错误 = 这个特性装不起来 —— 天空盒建不出管线,
        /// 场景就没有天空,那是上层该知道并决定的事(装个不产 pass 的
        /// SkyboxFeature 只会让问题在别处以「背景是黑的」出现)。
        [[nodiscard]] Expected<void> init(const Config& cfg);

    public:
        // Handle-based operations (pre-uploaded via ResourceSyncer)
        bool applyEquirectangularHandle(RTextureHandle texture, float rotation_radians, float intensity);
        bool applyCubemapHandles(RTextureHandle cube, float rotation_radians, float intensity);
        [[nodiscard]] SkyboxStatsReply stats() const noexcept;

    private:
        GraphicsPipelineHandle cubemap_handle_{kInvalidPipelineHandle};
        GraphicsPipelineHandle equirect_handle_{kInvalidPipelineHandle};

        // ------------------------------------------------------------------
        // Dynamic state
        // ------------------------------------------------------------------

        // ------------------------------------------------------------------
        // Equirectangular state (managed by TextureResources, binding 0)
        // ------------------------------------------------------------------
        uint32_t equirect_bindless_index_{0}; ///< Index into the bindless sampler2D[] array

        // ------------------------------------------------------------------
        // Cubemap state (managed by TextureResources, binding 1)
        // ------------------------------------------------------------------
        uint32_t cubemap_bindless_index_{0}; ///< Index into the bindless samplerCube[] array
        float rotation_radians_{0.0f};
        float intensity_{1.0f};

        // ------------------------------------------------------------------
        // Context: accessed via base-class renderContext() after attach
        // ------------------------------------------------------------------

        // ------------------------------------------------------------------
        // Which variant is active?
        // ------------------------------------------------------------------
        enum class ActiveMode
        {
            NONE,
            EQUIRECT,
            CUBEMAP
        };
        ActiveMode active_mode_{ActiveMode::NONE};
        std::uint32_t pass_visits_{0u};
        std::uint32_t draws_{0u};
        std::uint32_t inactive_pass_visits_{0u};
        std::uint32_t pipeline_bind_failures_{0u};

        // ------------------------------------------------------------------
        // Deferred command: texture wasn't ready when first drained.
        // ------------------------------------------------------------------
        Config cfg_{};
    };

} // namespace lux::render
