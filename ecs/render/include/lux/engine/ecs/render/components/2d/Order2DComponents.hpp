#pragma once
// ============================================================================
//  Order2DComponents.hpp — opt-in 2D draw-order modifiers (A2-03):
//  Y-sort and parallax, plus the world/UI band contract.
//
//  v2 translation of the checklist's "DrawOrderKey producer" wording: the
//  PRODUCER (bridge) derives an EFFECTIVE priority / bakes the parallax shift
//  into the transform it sends; the canvas only sorts keys. Protocol, shaders
//  and arena are untouched; authored Image2DComponent/TilemapComponent fields
//  are never written back (the modifiers are read-only inputs to the bake).
//
//  BAND CONTRACT (define-first, per the checklist): one shared priority axis,
//  partitioned by convention —
//     world content   priority <  kUiBandPriority   (authored / y-sorted)
//     UI / HUD        priority >= kUiBandPriority, Parallax2D factor (0,0)
//  factor-0 parallax pins content to the camera (an MVP HUD; camera ZOOM still
//  scales it — a true screen-space UI pass is a later, separate task).
// ============================================================================

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <Eigen/Core>
#include <cmath>
#include <limits>

namespace lux::ecs
{
    /// Everything at or above this priority is UI-band by convention.
    inline constexpr float kUiBandPriority = 1000.f;

    /// Opt-in: derive the draw priority from the entity's world Y so lower-on-
    /// screen images draw on top (the classic top-down depth illusion):
    ///     effective_priority = band − quantize(world_y) × scale
    /// `quantize_step` > 0 buckets world_y (floor to the step) so sub-step
    /// motion does not change the key — the order-rebuild damper; 0 = exact.
    struct LUX_COMPONENT() YSort2DComponent
    {
        LUX_MEMBER(display_name=Band, tooltip=Base priority the Y offset is applied to)
        float band{0.f};

        LUX_MEMBER(display_name=Scale, tooltip=Priority units per world unit of Y)
        float scale{1.f};

        LUX_MEMBER(display_name=Quantize Step, tooltip=Y bucket size in world units; 0 = exact)
        float quantize_step{0.f};

        [[nodiscard]] float effectivePriority(double world_y) const noexcept
        {
            if (!std::isfinite(world_y))
                return band;
            double y = world_y;
            if (quantize_step > 0.f)
            {
                const auto step = static_cast<double>(quantize_step);
                y = std::floor(y / step) * step;
            }
            const auto result = static_cast<double>(band) -
                y * static_cast<double>(scale);
            if (result <= -std::numeric_limits<float>::max())
                return -std::numeric_limits<float>::max();
            if (result >= std::numeric_limits<float>::max())
                return std::numeric_limits<float>::max();
            return static_cast<float>(result);
        }
    };

    /// Opt-in: scroll against the camera. The producer adds
    ///     cam_center × (1 − factor)
    /// to the baked translation — factor 1 = normal world content (no-op),
    /// factor 0 = pinned to the camera (HUD), 0<f<1 = background layers.
    /// Value-compared like every transform input: a static camera sends zero
    /// wire traffic; a moving one re-sends only the parallax-affected quads.
    struct LUX_COMPONENT() Parallax2DComponent
    {
        LUX_MEMBER(display_name=Factor, tooltip=Per-axis parallax factor; 1 = none, 0 = camera-locked)
        Eigen::Vector2f factor{1.f, 1.f};
    };

} // namespace lux::ecs
