#pragma once

#include <lux/engine/math/Position.hpp>
/**
 * @file EditorCamera2DController.hpp — the editor 2D viewport navigator.
 *
 * The dimensional sibling of EditorCamera3DController (user ruling 2026-07-11:
 * controllers are per-kind, selected by scene composition — never an if(kind)
 * inside one controller). Drives the EDITOR camera entity's Transform2D +
 * Camera2DComponent from input:
 *   - pan  : while pan_mode is active, mouse pixel deltas translate the camera
 *            (pixel → world via units_per_view_height / viewport height)
 *   - zoom : scroll wheel scales units_per_view_height (multiplicative, clamped)
 *   - reset: return to the attach-time home pose
 *
 * NO view push here: camera → view upload is the binding model
 * (RenderViewBindingComponent + Camera2DUploadSubsystem). This controller only
 * writes world-side data.
 */

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/components/Transform2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <algorithm>
#include <cmath>

namespace lux::editor
{
    using lux::ecs::Transform2DComponent;
    using lux::ecs::Camera2DComponent;
    using lux::input::ActionMapper;

    class EditorCamera2DController
    {
    public:
        struct ActionIds
        {
            std::uint32_t pan_mode{0};      ///< hold to pan (shares the 3D pan chord)
            std::uint32_t look_x{0};        ///< mouse delta X (raw pixels)
            std::uint32_t look_y{0};        ///< mouse delta Y (raw pixels)
            std::uint32_t zoom_scroll{0};   ///< wheel: zoom in/out
            std::uint32_t reset{0};         ///< return to home pose
        };

        struct Config
        {
            float zoom_step   = 1.15f;      ///< wheel notch multiplier
            float zoom_min    = 0.01f;      ///< units_per_view_height clamp
            float zoom_max    = 100000.f;
        };

        void attach(lux::meta::entity_id cam, lux::meta::EntityRegistry* reg)
        {
            entity_ = cam;
            reg_  = reg;
            if (reg_ && reg_->valid(entity_) &&
                (*reg_).all_of<Transform2DComponent, Camera2DComponent>(entity_))
            {
                home_pos_  = reg_->get<Transform2DComponent>(entity_).position;
                home_upvh_ = reg_->get<Camera2DComponent>(entity_).units_per_view_height;
            }
        }

        void setActionIds(const ActionIds& ids) noexcept { ids_ = ids; }

        /// @p content_h viewport pixel height — pixel→world pan scale.
        void tick(const ActionMapper& m, float /*dt*/, float content_h)
        {
            if (!reg_ || !reg_->valid(entity_) ||
                !(*reg_).all_of<Transform2DComponent, Camera2DComponent>(entity_))
                return;

            const auto& tc = reg_->get<Transform2DComponent>(entity_);
            auto& cc = reg_->get<Camera2DComponent>(entity_);

            if (m.active(ids_.pan_mode) && content_h > 0.f)
            {
                const float wpp = cc.units_per_view_height / content_h;   // world per pixel
                const float dx  = m.getValue(ids_.look_x).as1D();
                const float dy  = m.getValue(ids_.look_y).as1D();
                // Drag the WORLD with the cursor: camera moves opposite the drag.
                // Screen Y is down in the canonical render-client projection.
                reg_->patch<lux::ecs::Transform2DComponent>(
                    entity_,
                    [dx, dy, wpp](Transform2DComponent& transform)
                    {
                        transform.position.x -= static_cast<double>(dx * wpp);
                        transform.position.y += static_cast<double>(dy * wpp);
                    });
            }

            const float scroll = m.getValue(ids_.zoom_scroll).as1D();
            if (scroll != 0.f)
            {
                cc.units_per_view_height = std::clamp(
                    cc.units_per_view_height * std::pow(cfg_.zoom_step, -scroll),
                    cfg_.zoom_min, cfg_.zoom_max);
            }

            if (m.triggered(ids_.reset))
            {
                reg_->patch<lux::ecs::Transform2DComponent>(
                    entity_,
                    [this](Transform2DComponent& transform)
                    {
                        transform.position = home_pos_;
                    });
                cc.units_per_view_height = home_upvh_;
            }
        }

        [[nodiscard]] bool wantsCursorCapture() const noexcept { return false; }

    private:
        lux::meta::entity_id entity_{lux::meta::null_entity};
        lux::meta::EntityRegistry* reg_{nullptr};
        ActionIds            ids_{};
        Config               cfg_{};
        lux::math::Position2d home_pos_{};
        float                home_upvh_{10.f};
    };

} // namespace lux::editor
