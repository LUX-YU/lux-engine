#pragma once
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/renderer/RenderObjectExtraction.hpp>
#include <lux/engine/render/renderer/features/particle/ParticleSyncCommands.hpp>
#include <cstdint>
#include <string>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/resources/ParticleResources.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/function/visibility.h>
#include <vulkan/vulkan.h>
#include <vector>

namespace lux::render
{
    /**
     * @brief GPU-side particle feature with compute simulation + billboard draw.
     *
     * This is the **render-thread** component.  Game-thread code should
     * interact through `ParticleSyncClient`.
     *
     * Adds two render-graph passes ordered after ForwardMesh:
     *   1. **ParticleUpdate** (COMPUTE) — particle_update.comp simulates all
     *      particles in parallel (velocity integration, lifetime decay).
     *   2. **ParticleDraw** (GRAPHICS) — particle.vert + particle.frag render
     *      billboard quads with additive blending.
     *
     * Usage:
     * @code
     *   renderer.addFeature<ParticleFeature>();
     *   // ...
     *   ParticleSyncClient particles(scene.syncProducer(), particle_fid);
     *   particles.emitBurst(origin, speed, lifetime, size, color, count);
     *   particles.setDeltaTime(dt);
     *   particles.flushNextFrame();
     * @endcode
     */
    class LUX_FUNCTION_PUBLIC ParticleFeature : public RenderFeature
    {
    public:
        static constexpr phase_mask_t kExtractPhaseMask =
            phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::ForwardTrans));

        struct Config
        {
            ShaderHandle compute_shader{};
            ShaderHandle vertex_shader{};
            ShaderHandle fragment_shader{};
            std::string color_target{"SceneColor"};
            std::string depth_target{"SceneDepth"};
        };

        ParticleFeature();
        explicit ParticleFeature(Config cfg);

        std::string_view name() const override { return "ParticleEffect"; }
        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;

        // =====================================================================
        //  RenderFeature lifecycle
        // =====================================================================
        void extractRenderObjects(
            const RenderObjectExtractContext& ctx,
            std::vector<DrawPacket>& out_packets);
        void addPasses(RGBuilder& builder) override;

        // =====================================================================
        //  Per-frame feature phases
        // =====================================================================
        void onFrameBegin(const FeatureFrameContext& ctx) override;

    public:
        // Sync handler — public for generated auto-registration.
        void onSyncSetParticleParams(const SetParticleSimParamsCmd& cmd, const FeatureFrameContext& ctx) noexcept;

    private:
        void init(const Config& cfg);
        [[nodiscard]] ParticleResources* particleResources() noexcept;

        // Graphics pipeline handle (registered with PipelineManager)
        GraphicsPipelineHandle  draw_pipeline_handle_{kInvalidPipelineHandle};

        // Compute pipeline registered with PipelineManager
        ComputePipelineHandle compute_pipeline_handle_{};
        // Layout is owned by PipelineLayoutService.
        VkPipelineLayout compute_layout_    {VK_NULL_HANDLE};

        // Per-frame simulation parameters
        float      delta_time_{0.016f};
        float      gravity_   {9.8f};
        float      pending_delta_time_{0.016f};
        float      pending_gravity_   {9.8f};
        bool       has_pending_params_{false};

        // RG handle for the imported particle buffer, shared between
        // addPasses() (compute write) and draw pass (draw read).
        RGResourceHandle particle_buf_rg_{};

        Config   cfg_{};
    };

    // No-arg ctor defined out-of-class so Config{} is evaluated where the class is
    // complete (GCC 11/12 reject Config{} / {} as an in-class default argument).
    inline ParticleFeature::ParticleFeature() : ParticleFeature(Config{}) {}

} // namespace lux::render
