#pragma once
#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/meta/LuxObject.hpp>
#include <cmath>

namespace lux::pack
{
    // These live in the gameplay core (input + World) — bring them into scope.
    using lux::ecs::World;
    using lux::input::ActionMapper;
    using lux::input::ActionId;
    using lux::input::InvalidActionId;
    // -----------------------------------------------------------------------
    // Well-known action IDs used by CameraController.
    // Assigned at runtime via InputActionRegistry::registerAction().
    // -----------------------------------------------------------------------
    namespace camera_actions
    {
        inline ActionId MoveForward  = InvalidActionId;
        inline ActionId MoveBackward = InvalidActionId;
        inline ActionId MoveLeft     = InvalidActionId;
        inline ActionId MoveRight    = InvalidActionId;
        inline ActionId MoveUp       = InvalidActionId;
        inline ActionId MoveDown     = InvalidActionId;
        inline ActionId LookYaw      = InvalidActionId;
        inline ActionId LookPitch    = InvalidActionId;
        inline ActionId OrbitYaw     = InvalidActionId;
        inline ActionId OrbitPitch   = InvalidActionId;
        inline ActionId OrbitZoom    = InvalidActionId;
    } // namespace camera_actions

    // -----------------------------------------------------------------------

    /// A controller that drives a camera entity's TransformComponent from
    /// ActionMapper state.
    ///
    /// Modes:
    ///  - FPS    — WASD + mouse-delta look; pitch clamped to ±89°
    ///  - Orbit  — mouse-drag orbit around a focal point; scroll to zoom
    class CameraController
    {
    public:
        enum class Mode { FPS, Orbit };

        struct Config
        {
            float move_speed       = 5.f;   ///< units per second (FPS)
            float look_sensitivity = 0.1f;  ///< degrees per raw mouse unit
            float orbit_sensitivity= 0.3f;  ///< degrees per raw mouse unit
            float zoom_speed       = 1.f;   ///< units per scroll tick
            float pitch_min        = -89.f; ///< degrees
            float pitch_max        =  89.f; ///< degrees
            float orbit_dist_min   = 0.5f;
            float orbit_dist_max   = 500.f;
        };

        CameraController() = default;
        explicit CameraController(Mode mode, Config config = {})
            : mode_(mode), config_(config) {}

        // ------------------------------------------------------------------ //
        //  Setup                                                              //
        // ------------------------------------------------------------------ //

        void attach(lux::meta::entity_id entity, World& world)
        {
            entity_ = entity;
            world_  = &world;
            syncAnglesFromTransform();
        }

        void setMode(Mode m) { mode_ = m; }
        [[nodiscard]] Mode   mode()   const noexcept { return mode_;   }
        [[nodiscard]] Config& config()       noexcept { return config_; }

        // ------------------------------------------------------------------ //
        //  Per-frame update                                                   //
        // ------------------------------------------------------------------ //

        void tick(const ActionMapper& mapper, float dt)
        {
            if (!world_ || !world_->valid(entity_)) return;
            if (mode_ == Mode::FPS)    tickFPS(mapper, dt);
            else                       tickOrbit(mapper, dt);
        }

    private:
        // ------------------------------------------------------------------ //
        //  FPS mode                                                           //
        // ------------------------------------------------------------------ //

        void tickFPS(const ActionMapper& mapper, float dt)
        {
            auto& tc = world_->get<TransformComponent>(entity_);

            // --- Look ---
            float dyaw   = mapper.axis(camera_actions::LookYaw)   * config_.look_sensitivity;
            float dpitch = mapper.axis(camera_actions::LookPitch)  * config_.look_sensitivity;
            yaw_deg_   -= dyaw;
            pitch_deg_ -= dpitch;
            pitch_deg_  = std::clamp(pitch_deg_, config_.pitch_min, config_.pitch_max);

            const float toRad = 3.14159265f / 180.f;
            Eigen::Quaternionf qYaw(Eigen::AngleAxisf(yaw_deg_ * toRad, Eigen::Vector3f::UnitY()));
            Eigen::Quaternionf qPitch(Eigen::AngleAxisf(pitch_deg_ * toRad, Eigen::Vector3f::UnitX()));
            tc.rotation = (qYaw * qPitch).normalized();

            // --- Move ---
            Eigen::Vector3f dir = Eigen::Vector3f::Zero();
            if (mapper.active(camera_actions::MoveForward))  dir.z() -= 1.f;
            if (mapper.active(camera_actions::MoveBackward)) dir.z() += 1.f;
            if (mapper.active(camera_actions::MoveLeft))     dir.x() -= 1.f;
            if (mapper.active(camera_actions::MoveRight))    dir.x() += 1.f;
            if (mapper.active(camera_actions::MoveUp))       dir.y() += 1.f;
            if (mapper.active(camera_actions::MoveDown))     dir.y() -= 1.f;

            if (dir.squaredNorm() > 0.f) {
                dir = tc.rotation * dir.normalized();
                tc.position += dir * (config_.move_speed * dt);
            }

            tc.dirty = true;
        }

        // ------------------------------------------------------------------ //
        //  Orbit mode                                                         //
        // ------------------------------------------------------------------ //

        void tickOrbit(const ActionMapper& mapper, float dt)
        {
            auto& tc = world_->get<TransformComponent>(entity_);

            float dyaw   = mapper.axis(camera_actions::OrbitYaw)   * config_.orbit_sensitivity;
            float dpitch = mapper.axis(camera_actions::OrbitPitch)  * config_.orbit_sensitivity;
            float dzoom  = mapper.axis(camera_actions::OrbitZoom)   * config_.zoom_speed;

            yaw_deg_      -= dyaw;
            pitch_deg_    -= dpitch;
            pitch_deg_     = std::clamp(pitch_deg_, config_.pitch_min, config_.pitch_max);
            orbit_dist_   -= dzoom;
            orbit_dist_    = std::clamp(orbit_dist_, config_.orbit_dist_min, config_.orbit_dist_max);

            const float toRad = 3.14159265f / 180.f;
            Eigen::Quaternionf qYaw(Eigen::AngleAxisf(yaw_deg_ * toRad, Eigen::Vector3f::UnitY()));
            Eigen::Quaternionf qPitch(Eigen::AngleAxisf(pitch_deg_ * toRad, Eigen::Vector3f::UnitX()));
            tc.rotation = (qYaw * qPitch).normalized();

            // Position the camera at orbit_dist_ along its local +Z from target.
            Eigen::Vector3f back = tc.rotation * Eigen::Vector3f(0.f, 0.f, orbit_dist_);
            tc.position = orbit_target_ + back;
            tc.dirty    = true;
        }

        // ------------------------------------------------------------------ //
        //  Initialisation helper                                              //
        // ------------------------------------------------------------------ //

        void syncAnglesFromTransform()
        {
            if (!world_ || !world_->valid(entity_)) return;
            auto* tc = world_->tryGet<TransformComponent>(entity_);
            if (!tc) return;

            // Decompose existing rotation into yaw/pitch.
            Eigen::Vector3f euler = tc->rotation.toRotationMatrix()
                                    .eulerAngles(1, 0, 2); // YXZ
            yaw_deg_   = euler.x() * (180.f / 3.14159265f);
            pitch_deg_ = euler.y() * (180.f / 3.14159265f);
        }

        // ------------------------------------------------------------------ //
        //  State                                                              //
        // ------------------------------------------------------------------ //

        Mode      mode_         = Mode::FPS;
        Config    config_;
        World*    world_        = nullptr;
        lux::meta::entity_id entity_ = lux::meta::null_entity;

        float yaw_deg_      = 0.f;
        float pitch_deg_    = 0.f;
        float orbit_dist_   = 5.f;
        Eigen::Vector3f orbit_target_ = Eigen::Vector3f::Zero();
    };

} // namespace lux::pack
