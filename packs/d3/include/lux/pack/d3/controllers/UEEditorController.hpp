#pragma once
/**
 * @file UEEditorController.hpp
 * @brief UE-style editor viewport camera controller.
 *
 * Owns yaw / pitch / fly-speed state for one camera entity and drives that
 * entity's TransformComponent each frame from `ActionMapper` action values.
 *
 * Modes (priority high → low):
 *
 *   Fly   — RMB held. Mouse look (yaw / pitch) + WASD-along-view + QE-on-world-up.
 *           Scroll wheel multiplies fly speed (range clamped).
 *   Pan   — MMB held. Drag the orbit-target in screen-space; camera follows.
 *   Orbit — LMB held. Drag yaw / pitch around the orbit target.
 *   Idle  — none of the above; camera frozen.
 *
 * Single-shot triggers:
 *
 *   F (focus) — snap the camera to look at the currently selected entity at a
 *               distance proportional to its bounds. Uses a short ease-in lerp.
 *   End (reset) — return to the camera's `home` pose (recorded at attach()).
 *
 * Header-only to avoid adding a new .cpp + DLL boundary; the class is small.
 */

#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/ecs/World.hpp>
#include "../world/components/TransformComponent.hpp"
#include "../world/components/WorldTransformComponent.hpp"
#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <functional>

namespace lux::editor::actions
{
    // Forward-declarations of the ActionId statics from EditorActions.hpp.
    // We don't include that header here to keep the controller free of any
    // engine/editor dependency (the controller lives in function/gameplay,
    // a lower layer). Callers wire the IDs in by passing them to
    // `UEEditorController::setActionIds(...)`.
} // namespace lux::editor::actions

namespace lux::pack
{
    // These live in the gameplay core (input + World) — bring them into scope.
    using lux::ecs::World;
    using lux::input::ActionMapper;
    using lux::input::ActionId;
    using lux::input::InvalidActionId;
    /// UE-style editor camera controller. See file header for the mode
    /// reference. Header-only.
    class UEEditorController
    {
    public:
        // ── Action ID wiring ─────────────────────────────────────────────
        //
        // The controller does NOT hard-code action names — callers pass in
        // the resolved IDs after registering them with their own action
        // table. This keeps function/gameplay free of editor-tier deps.
        struct ActionIds
        {
            ActionId fly_mode      = InvalidActionId;
            ActionId orbit_mode    = InvalidActionId;
            ActionId pan_mode      = InvalidActionId;
            ActionId alt_modifier  = InvalidActionId;

            ActionId move_forward  = InvalidActionId;
            ActionId move_right    = InvalidActionId;
            ActionId move_up       = InvalidActionId;
            ActionId look_yaw      = InvalidActionId;
            ActionId look_pitch    = InvalidActionId;
            ActionId speed_scroll  = InvalidActionId;

            ActionId focus_selected = InvalidActionId;
            ActionId reset         = InvalidActionId;
        };

        struct Config
        {
            float fly_speed_base    = 5.f;   ///< m/s at scroll = 0
            float fly_speed_min     = 0.05f;
            float fly_speed_max     = 50.f;
            float fly_speed_scroll  = 1.1f;  ///< multiplier per wheel tick

            float look_sensitivity  = 0.12f; ///< deg per mouse-delta unit (raw axis)
            float orbit_sensitivity = 0.30f; ///< deg per mouse-delta unit
            float pan_speed         = 0.005f;///< units per mouse-delta unit (scaled by orbit_dist)

            float pitch_min         = -89.f;
            float pitch_max         =  89.f;
            float orbit_dist_min    = 0.5f;
            float orbit_dist_max    = 500.f;

            float focus_dist_factor = 2.5f;  ///< snap dist = bounding_radius * factor
            float focus_lerp_time   = 0.15f; ///< seconds
        };

        enum class Mode : uint8_t { Idle, Fly, Orbit, Pan };

        UEEditorController() = default;
        explicit UEEditorController(Config cfg) : cfg_(std::move(cfg)) {}

        /// Bind the controller to a camera entity in @p world. The entity
        /// must already carry a TransformComponent (the controller writes
        /// position / rotation each tick). Records the entity's current
        /// pose as `home` for End-reset.
        void attach(lux::meta::entity_id entity, World& world)
        {
            entity_ = entity;
            world_  = &world;
            if (world_ && world_->valid(entity_) &&
                world_->registry().all_of<TransformComponent>(entity_))
            {
                const auto& tc = world_->get<TransformComponent>(entity_);
                home_position_ = tc.position;
                home_rotation_ = tc.rotation;
                seedYawPitchFromRotation(tc.rotation);
            }
        }

        /// Wire the resolved action IDs from the editor-side table.
        /// Call once after `EditorActions::registerAll`.
        void setActionIds(const ActionIds& ids) noexcept { ids_ = ids; }

        Config& config()       noexcept { return cfg_; }
        const Config& config() const noexcept { return cfg_; }

        /// Pull the focal point for F-focus / pan / orbit from a selection
        /// provider. Returning `null_entity` disables F-focus (no-op).
        using SelectionProvider = std::function<lux::meta::entity_id()>;
        void setSelectionProvider(SelectionProvider fn) { sel_fn_ = std::move(fn); }

        /// True only while RMB is held → caller (LuxEditor) uses this to
        /// flip GLFW into cursor-disabled + raw-mouse-motion mode.
        [[nodiscard]] bool wantsCursorCapture() const noexcept
        {
            return mode_ == Mode::Fly;
        }

        /// Per-frame tick. Reads @p m action axes / button states; updates
        /// the bound camera entity's TransformComponent.
        void tick(const ActionMapper& m, float dt)
        {
            if (!world_ || !world_->valid(entity_) ||
                !world_->registry().all_of<TransformComponent>(entity_))
                return;

            const bool fly    = m.active(ids_.fly_mode);
            const bool orbit  = m.active(ids_.orbit_mode);
            const bool pan    = m.active(ids_.pan_mode);
            // Alt currently unused (reserved for Alt+LMB orbit / Alt+RMB
            // dolly); kept in ActionIds for forward compat.

            const Mode prev_mode = mode_;
            if      (fly)   mode_ = Mode::Fly;
            else if (pan)   mode_ = Mode::Pan;
            else if (orbit) mode_ = Mode::Orbit;
            else            mode_ = Mode::Idle;

            // ── Mode transition seeding ───────────────────────────────────
            //
            // The first frame of a new mode must reconstruct controller
            // state from the camera's current pose so the camera does NOT
            // teleport. This was the root cause of "clicking auto-focuses
            // toward origin": Orbit (and Pan) overwrite `tc.position` with
            // `orbit_target_ + back`, and if `orbit_target_` was still its
            // default (0,0,0), the very first frame of LMB-press would
            // snap the camera near origin.
            //
            // Seeding order:
            //   1. yaw / pitch from current rotation (Fly + Orbit both
            //      compose rotation from these)
            //   2. orbit target + distance from current view (Orbit + Pan
            //      both use these as the focal point)
            if (mode_ != prev_mode)
            {
                const auto& tc = world_->get<TransformComponent>(entity_);
                if (mode_ == Mode::Fly || mode_ == Mode::Orbit)
                    seedYawPitchFromRotation(tc.rotation);
                if (mode_ == Mode::Orbit || mode_ == Mode::Pan)
                    seedOrbitTargetFromCurrentView();
            }

            // Snap (lerp) animation has priority over mode tick — F / End
            // should feel uninterruptible, like UE.
            if (snap_.active)
            {
                tickSnap(dt);
                writeBack();
                return;
            }

            // Hotkeys: trigger-pulse semantics (one frame on press).
            if (m.triggered(ids_.focus_selected)) focusSelected();
            if (m.triggered(ids_.reset))          resetToHome();

            switch (mode_)
            {
            case Mode::Fly:   tickFly  (m, dt); break;
            case Mode::Orbit: tickOrbit(m, dt); break;
            case Mode::Pan:   tickPan  (m, dt); break;
            case Mode::Idle:                    break;
            }
            writeBack();
        }

        // ── Read-only state queries (for HUD / debug) ────────────────────

        [[nodiscard]] Mode  mode()      const noexcept { return mode_;       }
        [[nodiscard]] float flySpeed()  const noexcept { return fly_speed_;  }
        [[nodiscard]] float orbitDist() const noexcept { return orbit_dist_; }
        [[nodiscard]] const Eigen::Vector3f& orbitTarget() const noexcept { return orbit_target_; }

    private:
        // -----------------------------------------------------------------
        //  Mode bodies
        // -----------------------------------------------------------------

        void tickFly(const ActionMapper& m, float dt)
        {
            // Mouse look — accumulates raw pixel-delta * sensitivity.
            const float dyaw   = m.getValue(ids_.look_yaw  ).as1D() * cfg_.look_sensitivity;
            const float dpitch = m.getValue(ids_.look_pitch).as1D() * cfg_.look_sensitivity;
            yaw_deg_   -= dyaw;
            pitch_deg_ -= dpitch;
            pitch_deg_  = std::clamp(pitch_deg_, cfg_.pitch_min, cfg_.pitch_max);

            // Scroll wheel → speed (multiplicative; clamped).
            const float scroll = m.getValue(ids_.speed_scroll).as1D();
            if (scroll != 0.f)
            {
                fly_speed_ *= std::pow(cfg_.fly_speed_scroll, scroll);
                fly_speed_  = std::clamp(fly_speed_, cfg_.fly_speed_min, cfg_.fly_speed_max);
            }

            // WASD / QE → move in camera-relative basis (Y = world up).
            const auto rot = composeRotation();
            const Eigen::Vector3f fwd   = rot * Eigen::Vector3f(0.f, 0.f, -1.f);
            const Eigen::Vector3f right = rot * Eigen::Vector3f::UnitX();
            const Eigen::Vector3f up    = Eigen::Vector3f::UnitY();

            Eigen::Vector3f move = Eigen::Vector3f::Zero();
            move += fwd   * m.getValue(ids_.move_forward).as1D();
            move += right * m.getValue(ids_.move_right  ).as1D();
            move += up    * m.getValue(ids_.move_up     ).as1D();

            auto& tc = world_->get<TransformComponent>(entity_);
            tc.rotation = rot;
            if (move.squaredNorm() > 1e-8f)
                tc.position += move.normalized() * fly_speed_ * dt;
            tc.dirty = true;
        }

        void tickOrbit(const ActionMapper& m, float dt)
        {
            (void)dt;
            const float dyaw   = m.getValue(ids_.look_yaw  ).as1D() * cfg_.orbit_sensitivity;
            const float dpitch = m.getValue(ids_.look_pitch).as1D() * cfg_.orbit_sensitivity;
            yaw_deg_   -= dyaw;
            pitch_deg_ -= dpitch;
            pitch_deg_  = std::clamp(pitch_deg_, cfg_.pitch_min, cfg_.pitch_max);

            // Orbit target stays put; reposition the camera around it.
            const auto rot = composeRotation();
            const Eigen::Vector3f back =
                rot * Eigen::Vector3f(0.f, 0.f, orbit_dist_);
            auto& tc = world_->get<TransformComponent>(entity_);
            tc.rotation = rot;
            tc.position = orbit_target_ + back;
            tc.dirty    = true;
        }

        void tickPan(const ActionMapper& m, float dt)
        {
            (void)dt;
            const float dx = m.getValue(ids_.look_yaw  ).as1D();
            const float dy = m.getValue(ids_.look_pitch).as1D();
            if (dx == 0.f && dy == 0.f) return;

            const auto rot = composeRotation();
            const Eigen::Vector3f right = rot * Eigen::Vector3f::UnitX();
            const Eigen::Vector3f up    = rot * Eigen::Vector3f::UnitY();

            const float speed = cfg_.pan_speed * orbit_dist_;
            orbit_target_ += right * (-dx * speed) + up * (dy * speed);

            // Keep the camera looking at the new target by sliding it along
            // the existing view axis.
            const Eigen::Vector3f back = rot * Eigen::Vector3f(0.f, 0.f, orbit_dist_);
            auto& tc = world_->get<TransformComponent>(entity_);
            tc.position = orbit_target_ + back;
            tc.dirty    = true;
        }

        // -----------------------------------------------------------------
        //  Hotkeys
        // -----------------------------------------------------------------

        void focusSelected()
        {
            if (!sel_fn_) return;
            const auto sel = sel_fn_();
            if (sel == lux::meta::null_entity) return;
            auto& reg = world_->registry();
            if (!reg.all_of<WorldTransformComponent>(sel)) return;

            const auto& wt = reg.get<WorldTransformComponent>(sel);
            const Eigen::Vector3f center = wt.world.template block<3, 1>(0, 3);

            // Without per-entity bounds plumbed all the way to gameplay we
            // approximate the focus distance from the world transform's
            // diagonal scale: 1× scale → ~1m mesh → ~2.5m focus distance.
            // The full version (mesh local-AABB diagonal × world-max-scale)
            // lands together with picking precision in F2 stage B.
            float scale_proxy = wt.world.template block<3, 3>(0, 0)
                                       .colwise().norm().maxCoeff();
            if (scale_proxy < 0.01f) scale_proxy = 1.f;
            const float dist = std::max(scale_proxy * cfg_.focus_dist_factor,
                                        cfg_.orbit_dist_min);

            orbit_target_ = center;
            orbit_dist_   = dist;

            const auto rot      = composeRotation();
            const Eigen::Vector3f back = rot * Eigen::Vector3f(0.f, 0.f, dist);
            const Eigen::Vector3f tgt  = center + back;

            const auto& tc = world_->get<TransformComponent>(entity_);
            snapTo(tc.position, tc.rotation, tgt, rot, cfg_.focus_lerp_time);
        }

        void resetToHome()
        {
            // Re-seed yaw / pitch so post-reset look modes don't snap from
            // a stale internal state.
            seedYawPitchFromRotation(home_rotation_);
            orbit_target_ = home_position_
                          + home_rotation_ * Eigen::Vector3f(0.f, 0.f, -orbit_dist_);
            const auto& tc = world_->get<TransformComponent>(entity_);
            snapTo(tc.position, tc.rotation,
                   home_position_, home_rotation_, cfg_.focus_lerp_time);
        }

        // -----------------------------------------------------------------
        //  Snap-to lerp (used by F-focus and End-reset)
        // -----------------------------------------------------------------

        struct Snap
        {
            bool                 active   = false;
            float                t        = 0.f;
            float                duration = 0.15f;
            Eigen::Vector3f      from_p;
            Eigen::Vector3f      to_p;
            Eigen::Quaternionf   from_r;
            Eigen::Quaternionf   to_r;
        } snap_;

        void snapTo(const Eigen::Vector3f&     from_p,
                    const Eigen::Quaternionf&  from_r,
                    const Eigen::Vector3f&     to_p,
                    const Eigen::Quaternionf&  to_r,
                    float                      duration)
        {
            snap_.active   = true;
            snap_.t        = 0.f;
            snap_.duration = std::max(duration, 1e-3f);
            snap_.from_p   = from_p;
            snap_.from_r   = from_r;
            snap_.to_p     = to_p;
            snap_.to_r     = to_r;
        }

        void tickSnap(float dt)
        {
            snap_.t += dt;
            const float u = std::clamp(snap_.t / snap_.duration, 0.f, 1.f);
            // Smoothstep ease so the head / tail of the lerp are gentle.
            const float s = u * u * (3.f - 2.f * u);
            auto& tc = world_->get<TransformComponent>(entity_);
            tc.position = snap_.from_p * (1.f - s) + snap_.to_p * s;
            tc.rotation = snap_.from_r.slerp(s, snap_.to_r);
            tc.dirty    = true;
            if (u >= 1.f)
            {
                snap_.active = false;
                seedYawPitchFromRotation(snap_.to_r);
            }
        }

        // -----------------------------------------------------------------
        //  Helpers
        // -----------------------------------------------------------------

        /// Build the current camera rotation from yaw_deg_ / pitch_deg_.
        [[nodiscard]] Eigen::Quaternionf composeRotation() const noexcept
        {
            constexpr float kDeg2Rad = 3.14159265358979323846f / 180.f;
            Eigen::Quaternionf qYaw  (Eigen::AngleAxisf(yaw_deg_   * kDeg2Rad,
                                                       Eigen::Vector3f::UnitY()));
            Eigen::Quaternionf qPitch(Eigen::AngleAxisf(pitch_deg_ * kDeg2Rad,
                                                       Eigen::Vector3f::UnitX()));
            return (qYaw * qPitch).normalized();
        }

        /// Decompose a quaternion to (yaw_deg, pitch_deg) such that
        /// composeRotation() of the result reproduces the input (within
        /// numerical tolerance, with pitch clamped to ±90°).
        void seedYawPitchFromRotation(const Eigen::Quaternionf& q) noexcept
        {
            const Eigen::Vector3f fwd = q * Eigen::Vector3f(0.f, 0.f, -1.f);
            constexpr float kRad2Deg = 180.f / 3.14159265358979323846f;
            pitch_deg_ = std::asin(std::clamp(fwd.y(), -1.f, 1.f)) * kRad2Deg;
            yaw_deg_   = std::atan2(-fwd.x(), -fwd.z()) * kRad2Deg;
        }

        /// Initialize `orbit_target_` consistently with the camera's current
        /// position / rotation so the very first tick of Orbit / Pan does
        /// NOT teleport the camera.
        ///
        /// Math: `tickOrbit` writes
        ///       tc.position = orbit_target_ + rot * (0,0,orbit_dist_)
        ///                   = orbit_target_ - view_forward * orbit_dist_
        ///       so for `tc.position` to equal the *current* position we
        ///       must set `orbit_target_ = position + view_forward * orbit_dist_`.
        ///       Any other anchor (e.g. the selected entity's location)
        ///       would teleport the camera unless it happens to already lie
        ///       on the current view ray at distance `orbit_dist_`.
        ///
        /// `orbit_dist_` is kept from the previous orbit / F-focus session
        /// (default 5m at construction). To orbit *around* the selected
        /// entity, press F first — that updates both `orbit_target_` and
        /// `orbit_dist_` so the camera is looking at the entity, after
        /// which this seeder reproduces that anchor on the next orbit
        /// entry.
        void seedOrbitTargetFromCurrentView() noexcept
        {
            if (!world_ || !world_->valid(entity_)) return;
            const auto& tc = world_->get<TransformComponent>(entity_);
            const Eigen::Vector3f fwd =
                tc.rotation * Eigen::Vector3f(0.f, 0.f, -1.f);
            orbit_target_ = tc.position + fwd * orbit_dist_;
        }

        void writeBack() noexcept
        {
            // No-op for now — every tick mode writes through TransformComponent
            // directly. Kept as a single seam so a future "deferred apply"
            // refactor doesn't have to chase every mode body.
        }

        // -----------------------------------------------------------------
        //  State
        // -----------------------------------------------------------------

        Config              cfg_{};
        ActionIds           ids_{};
        World*              world_  = nullptr;
        lux::meta::entity_id entity_ = lux::meta::null_entity;
        SelectionProvider   sel_fn_;

        Mode                mode_ = Mode::Idle;

        float               yaw_deg_     = 0.f;
        float               pitch_deg_   = 0.f;
        float               fly_speed_   = 5.f;
        float               orbit_dist_  = 5.f;
        Eigen::Vector3f     orbit_target_ = Eigen::Vector3f::Zero();

        Eigen::Vector3f     home_position_ = Eigen::Vector3f::Zero();
        Eigen::Quaternionf  home_rotation_ = Eigen::Quaternionf::Identity();
    };

} // namespace lux::pack
