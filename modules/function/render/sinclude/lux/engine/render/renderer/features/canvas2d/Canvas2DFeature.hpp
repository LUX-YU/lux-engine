#pragma once
/**
 * @file Canvas2DFeature.hpp
 * @brief The SinglePerScene GPU-driven 2D canvas (v2 — .internal/2d-gpu-driven-rewrite.md).
 *
 * Sprites are GPU-RESIDENT instances in the scene's Canvas2DInstanceArena
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
#include <lux/engine/render/core/ResourceHandle.hpp>               // ShaderHandle
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp> // GraphicsPipelineHandle
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <string>

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
            ShaderHandle  vertex_shader{};
            ShaderHandle  fragment_shader{};
        };

        Canvas2DFeature();
        explicit Canvas2DFeature(Config cfg);
        ~Canvas2DFeature() override;

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;
        void addPasses(RGBuilder& builder) override;
        void onDetachFromScene(RenderScene& scene) override;

        /// The scene's instance arena (test/demo observability; null before attach).
        [[nodiscard]] const Canvas2DInstanceArena* arena() const noexcept { return arena_; }

    private:
        Config                 cfg_;
        GraphicsPipelineHandle pipeline_handle_{kInvalidPipelineHandle};        ///< variant 0: sprite kind
        GraphicsPipelineHandle field_pipeline_handle_{kInvalidPipelineHandle};  ///< variant 1: pixel-field kind
        GraphicsPipelineHandle tile_pipeline_handle_{kInvalidPipelineHandle};   ///< variant 2: tile kind
        Canvas2DInstanceArena* arena_{nullptr};
    };

    // No-arg ctor defined out-of-class so Config{} is evaluated where the class is
    // complete (GCC 11/12 reject Config{} / {} as an in-class default argument).
    inline Canvas2DFeature::Canvas2DFeature() : Canvas2DFeature(Config{}) {}

} // namespace lux::render
