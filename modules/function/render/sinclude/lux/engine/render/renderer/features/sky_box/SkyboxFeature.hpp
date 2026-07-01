#pragma once
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/renderer/features/sky_box/SkyboxSyncCommands.hpp>
#include <cstdint>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
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

        std::string_view name() const override { return "Skybox"; }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;

        // ---- RenderFeature lifecycle (render thread) ----
        void addPasses(RGBuilder& builder) override;

        void onFrameBegin(const FeatureFrameContext& ctx) override;

        bool isLoaded() const noexcept { return active_mode_ != ActiveMode::NONE; }

    private:
        void init(const Config& cfg);

    public:
        // Handle-based operations (pre-uploaded via ResourceSyncer)
        bool applyEquirectangularHandle(RTextureHandle texture);
        bool applyCubemapHandles(RTextureHandle cube);

        // Sync handlers — public for generated auto-registration.
        void onSetEquirectHandle(const SetEquirectHandleCmd& cmd, const FeatureFrameContext& ctx) noexcept;
        void onSetCubemapHandles(const SetCubemapHandlesCmd& cmd, const FeatureFrameContext& ctx) noexcept;

    private:
        GraphicsPipelineHandle cubemap_handle_{kInvalidPipelineHandle};
        GraphicsPipelineHandle equirect_handle_{kInvalidPipelineHandle};

        // ------------------------------------------------------------------
        // Dynamic state
        // ------------------------------------------------------------------

        // ------------------------------------------------------------------
        // Equirectangular state (managed by TextureResources, binding 0)
        // ------------------------------------------------------------------
        uint32_t      equirect_bindless_index_{0};   ///< Index into the bindless sampler2D[] array

        // ------------------------------------------------------------------
        // Cubemap state (managed by TextureResources, binding 1)
        // ------------------------------------------------------------------
        uint32_t      cubemap_bindless_index_{0};     ///< Index into the bindless samplerCube[] array

        // ------------------------------------------------------------------
        // Context: accessed via base-class renderContext() after attach
        // ------------------------------------------------------------------

        // ------------------------------------------------------------------
        // Which variant is active?
        // ------------------------------------------------------------------
        enum class ActiveMode { NONE, EQUIRECT, CUBEMAP };
        ActiveMode active_mode_{ActiveMode::NONE};

        // ------------------------------------------------------------------
        // Deferred command: texture wasn't ready when first drained.
        // Retried every frame until the pool reports the handle as ready.
        // ------------------------------------------------------------------
        std::optional<SkyboxSyncCmd> pending_cmd_;
        Config cfg_{};
    };

} // namespace lux::render
