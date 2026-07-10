#pragma once
#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/ecs/World.hpp>
#include <cmath>

namespace lux::pack
{
    // These live in the gameplay core (input + World) — bring them into scope.
    using lux::ecs::World;
    using lux::input::ActionMapper;
    using lux::input::ActionId;
    using lux::input::InvalidActionId;
    // -----------------------------------------------------------------------
    // Well-known action IDs used by EditorController.
    // Assigned at runtime via InputActionRegistry::registerAction().
    // -----------------------------------------------------------------------
    namespace editor_actions
    {
        inline ActionId OrbitYaw   = InvalidActionId;
        inline ActionId OrbitPitch = InvalidActionId;
        inline ActionId OrbitZoom  = InvalidActionId;
        inline ActionId PanX       = InvalidActionId;
        inline ActionId PanY       = InvalidActionId;
    }

    /// A simplified orbit-only camera controller for editor viewports.
    ///
    /// Supports:
    ///  - Orbit (yaw/pitch) around a focal point
    ///  - Zoom (scroll wheel)
    ///  - Screen-space pan (middle-mouse drag)
    class EditorController
    {
    public:
        struct Config
        {
            float orbit_sensitivity = 0.3f;  ///< degrees per raw mouse unit
            float pan_speed         = 0.01f; ///< units per raw mouse unit (scaled by dist)
            float zoom_speed        = 1.f;   ///< units per scroll tick
            float pitch_min         = -89.f;
            float pitch_max         =  89.f;
            float orbit_dist_min    = 0.5f;
            float orbit_dist_max    = 500.f;
        };

        EditorController() = default;
        explicit EditorController(Config config) : config_(config) {}

        void attach(lux::meta::entity_id entity, World& world)
        {
            entity_ = entity;
            world_  = &world;
        }

        void setTarget(const Eigen::Vector3f& target) { orbit_target_ = target; }
        [[nodiscard]] const Eigen::Vector3f& target() const noexcept { return orbit_target_; }

        Config& config() noexcept { return config_; }

        void tick(const ActionMapper& mapper, float dt)
        {
            if (!world_ || !world_->valid(entity_)) return;
            auto& tc = world_->get<TransformComponent>(entity_);

            // --- Orbit ---
            float dyaw   = mapper.axis(editor_actions::OrbitYaw)   * config_.orbit_sensitivity;
            float dpitch = mapper.axis(editor_actions::OrbitPitch)  * config_.orbit_sensitivity;
            yaw_deg_   -= dyaw;
            pitch_deg_ -= dpitch;
            pitch_deg_  = std::clamp(pitch_deg_, config_.pitch_min, config_.pitch_max);

            // --- Zoom ---
            float dzoom = mapper.axis(editor_actions::OrbitZoom) * config_.zoom_speed;
            orbit_dist_ -= dzoom;
            orbit_dist_  = std::clamp(orbit_dist_, config_.orbit_dist_min, config_.orbit_dist_max);

            const float toRad = 3.14159265f / 180.f;
            Eigen::Quaternionf qYaw(Eigen::AngleAxisf(yaw_deg_ * toRad, Eigen::Vector3f::UnitY()));
            Eigen::Quaternionf qPitch(Eigen::AngleAxisf(pitch_deg_ * toRad, Eigen::Vector3f::UnitX()));
            tc.rotation = (qYaw * qPitch).normalized();

            // --- Pan — move the focal target in camera's local XY ---
            float pan_x = mapper.axis(editor_actions::PanX) * config_.pan_speed * orbit_dist_;
            float pan_y = mapper.axis(editor_actions::PanY) * config_.pan_speed * orbit_dist_;
            if (pan_x != 0.f || pan_y != 0.f) {
                Eigen::Vector3f right = tc.rotation * Eigen::Vector3f::UnitX();
                Eigen::Vector3f up    = tc.rotation * Eigen::Vector3f::UnitY();
                orbit_target_ +=  right * (-pan_x) + up * pan_y;
            }

            Eigen::Vector3f back = tc.rotation * Eigen::Vector3f(0.f, 0.f, orbit_dist_);
            tc.position = orbit_target_ + back;
            tc.dirty    = true;
        }

    private:
        Config   config_;
        World*   world_  = nullptr;
        lux::meta::entity_id entity_ = lux::meta::null_entity;

        float yaw_deg_    = 0.f;
        float pitch_deg_  = 0.f;
        float orbit_dist_ = 5.f;
        Eigen::Vector3f orbit_target_ = Eigen::Vector3f::Zero();
    };

} // namespace lux::pack
