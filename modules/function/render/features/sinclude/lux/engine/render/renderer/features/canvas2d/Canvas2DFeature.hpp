#pragma once
/**
 * @file Canvas2DFeature.hpp
 * @brief The SinglePerScene GPU-driven 2D canvas (v2 — .internal/2d-gpu-driven-rewrite.md).
 *
 * Images are GPU-RESIDENT instances in the scene's Canvas2DInstanceArena
 * (scene-registry-owned; this feature attaches/initializes it and registers it
 * as a transfer contributor). The wire carries create/destroy/delta commands —
 * never per-frame content — so a static scene costs zero on every hop.
 *
 * Drawing is ONE fixed color-only pass to `SceneColor` (no depth; premultiplied
 * alpha) with ONE instanced draw: `vkCmdDraw(6, arena.drawCount(), 0, 0)`. The
 * vertex shader PULLS everything (no vertex input state at all): set 1 binding 0
 * = the instance records, binding 1 = the slot order (ascending sort key ⇒ the
 * instance index sequence IS the painter order); set 0 = the standard per-view
 * camera; set 2 = the global bindless textures. Graph topology never changes
 * with content — the arena's streams update via the transfer phase.
 */

#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>               // ShaderHandle
#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp> // GraphicsPipelineHandle
#include <lux/engine/render/gpu/lifecycle/FifOwned.hpp>      // group_sampler_ (A2-04)
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>

namespace lux::render
{
    class Canvas2DInstanceArena;   // scene-registry instance store (Canvas2DInstanceArena.hpp)

    class LUX_FUNCTION_PUBLIC Canvas2DFeature final : public RenderFeature
    {
    public:
        struct Config
        {
            std::string   name{"Canvas2D"};
            std::string   color_target{"SceneColor"};   ///< the pass writes here (color-only)
            std::uint32_t initial_capacity{4096};       ///< arena slots at attach (grows ×2)
            std::uint32_t max_capacity{65536};          ///< growth ceiling (CapacityExhausted beyond)
            /// A2-04: offscreen groups (0 = none — EXACTLY the pre-A2-04
            /// topology, zero extra RTs/passes/pipelines). Each declared group
            /// g∈[1..kMaxCanvas2DGroups] renders into its own full-view RT and
            /// is premultiplied-composited onto color_target afterwards.
            /// Declared ONCE at creation — the pass topology stays fixed.
            std::uint32_t offscreen_groups{0};
            ShaderHandle  vertex_shader{};
            ShaderHandle  fragment_shader{};
        };

        Canvas2DFeature();
        explicit Canvas2DFeature(Config cfg);
        ~Canvas2DFeature() override;

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;
        void onDetachFromScene(RenderScene& scene) override;
        [[nodiscard]] bool canRebaseSceneOrigin(
            const std::int64_t origin_delta[3]) const noexcept override;
        void rebaseSceneOrigin(
            const std::int64_t origin_delta[3]) noexcept override;

        /// The scene's instance arena (test/demo observability; null before attach).
        [[nodiscard]] const Canvas2DInstanceArena* arena() const noexcept { return arena_; }

    private:
        Config                 cfg_;
        GraphicsPipelineHandle pipeline_handle_{kInvalidPipelineHandle};        ///< variant 0: image kind
        GraphicsPipelineHandle field_pipeline_handle_{kInvalidPipelineHandle};  ///< variant 1: pixel-field kind
        GraphicsPipelineHandle tile_pipeline_handle_{kInvalidPipelineHandle};   ///< variant 2: tile kind
        // A2-04 (only when offscreen_groups > 0):
        GraphicsPipelineHandle composite_pipeline_{kInvalidPipelineHandle};     ///< group RT → color_target
        VkDescriptorSetLayout  composite_ds_layout_{VK_NULL_HANDLE};            ///< set 1: one sampler
        VkSampler              group_sampler_{VK_NULL_HANDLE};                  ///< 服务缓存句柄(线性 clamp)
        Canvas2DInstanceArena* arena_{nullptr};
    };

    // No-arg ctor defined out-of-class so Config{} is evaluated where the class is
    // complete (GCC 11/12 reject Config{} / {} as an in-class default argument).
    inline Canvas2DFeature::Canvas2DFeature() : Canvas2DFeature(Config{}) {}

} // namespace lux::render
