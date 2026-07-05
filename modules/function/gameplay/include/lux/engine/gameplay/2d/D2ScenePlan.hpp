#pragma once
// ============================================================================
//  D2ScenePlan.hpp — capability collection for the 2D kit (lux::gameplay::d2).
//
//  A plan COLLECTS which optional capabilities a scene wants; d2::install()
//  (Scene2D.hpp) turns one plan into ONE deterministic system order in a single
//  addSystem pass. This indirection is deliberate (design §2.5, 3rd-review
//  blocker): the neutral World is append-only — addSystem appends in registration
//  order with no remove/replace/reorder (World.hpp) — so fine-grained
//  installXxx(World&) calls could neither produce the required order (2D physics
//  must run BEFORE Transform2D, unlike d3's Transform→Camera→Animation) nor demote
//  an already-registered system into an internal phase of a later coordinator. A
//  plan sidesteps both: the system set + order are decided ONCE, at install().
//
//  See .internal/lux-engine-2d-pack-design.md §2.4 (fixed-step) + §2.5 (install).
// ============================================================================

#include <cstdint>

namespace lux::gameplay::d2
{
    /// Fixed-step configuration for the unified Simulation2DSystem accumulator
    /// (design §2.4). Every fixed-step capability (pixel sim, physics) shares ONE
    /// accumulator, so this lives on the plan, not per-system.
    struct FixedStepConfig
    {
        float fixed_dt        = 1.0f / 60.0f;  ///< the fixed timestep
        int   max_substeps    = 4;             ///< spiral-of-death guard (cap substeps/frame)
        float max_accumulated = 0.25f;         ///< clamp on banked time (long-stall catch-up guard)
        /// On hitting max_substeps with time still banked, DISCARD the backlog
        /// (fmod) rather than carrying it — the only MVP drop policy. Kept as a
        /// field so a future "catch-up" variant is a config change, not a rewrite.
        bool  drop_excess_time = true;
    };

    /// Per-capability configs. Placeholders for now — their owning tasks (physics /
    /// pixel-sim) flesh them out; the plan just carries them by value so a caller
    /// can configure at enable*() time.
    struct Physics2DConfig  {};
    struct PixelSimConfig   {};

    /// The optional capabilities a 2D scene may enable. `Core` (Transform2D +
    /// Camera2D) is the L1 base every 2D scene needs; the rest are opt-in layers
    /// that pay nothing when absent (design §1.3 payment symmetry).
    enum class D2Capability : std::uint32_t
    {
        None                = 0u,
        Core                = 1u << 0,   ///< Transform2D + Camera2D (base)
        SpriteAnimation     = 1u << 1,   ///< SpriteAnimSystem (consumes dt)
        Physics             = 1u << 2,   ///< collision + rigid/character (fixed-step)
        CharacterController = 1u << 3,   ///< kinematic sweep (needs collision)
        PixelSimulation     = 1u << 4,   ///< CA field stepping (fixed-step; needs a runtime)
        PixelInterop        = 1u << 5,   ///< query/command/event/transfer seam (needs Physics ∧ PixelSimulation)
    };

    [[nodiscard]] constexpr std::uint32_t operator|(D2Capability a, D2Capability b) noexcept
    { return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b); }

    /// Why a plan is invalid (bit flags, so validate() can report EVERY violation at
    /// once — a config-matrix test wants all of them). A malformed plan must be caught
    /// as a structured result, never an assert (user config error ≠ programmer bug).
    enum class D2PlanError : std::uint32_t
    {
        None                                = 0u,
        MissingCore                         = 1u << 0,  ///< a non-empty plan without Core (every layer is placed by Transform2D)
        PixelInteropRequiresPhysics         = 1u << 1,  ///< the pixel↔rigid seam needs a physics world
        PixelInteropRequiresPixelSimulation = 1u << 2,  ///< …and a pixel field to transfer with
        CharacterControllerRequiresPhysics  = 1u << 3,  ///< kinematic sweep runs over the collision world
        FixedStepInvalidDt                  = 1u << 4,  ///< fixed_dt must be > 0
        FixedStepInvalidSubsteps            = 1u << 5,  ///< max_substeps must be >= 1
        FixedStepInvalidAccumulated         = 1u << 6,  ///< max_accumulated must bank at least one step (>= fixed_dt)
    };

    /// Structured validation result — deterministic (same plan → same errors), no
    /// allocation, no assert.
    struct D2PlanValidation
    {
        std::uint32_t errors = 0u;
        [[nodiscard]] bool ok()  const noexcept { return errors == 0u; }
        [[nodiscard]] bool has(D2PlanError e) const noexcept
        { return (errors & static_cast<std::uint32_t>(e)) != 0u; }
    };

    /// Collects capabilities + configs; consumed once by d2::install(). Pure value
    /// type (copyable), no World/render coupling — the install functions do the wiring.
    class D2ScenePlan
    {
    public:
        D2ScenePlan& enableCore()                                        { set(D2Capability::Core);                return *this; }
        D2ScenePlan& enableSpriteAnimation()                             { set(D2Capability::SpriteAnimation);     return *this; }
        D2ScenePlan& enablePhysics(const Physics2DConfig& cfg = {})      { set(D2Capability::Physics); physics_ = cfg; return *this; }
        D2ScenePlan& enableCharacterController()                         { set(D2Capability::CharacterController); return *this; }
        D2ScenePlan& enablePixelSimulation(const PixelSimConfig& cfg = {}) { set(D2Capability::PixelSimulation); pixel_ = cfg; return *this; }
        D2ScenePlan& enablePixelInterop()                               { set(D2Capability::PixelInterop);        return *this; }

        D2ScenePlan& setFixedStep(const FixedStepConfig& cfg) noexcept   { fixed_step_ = cfg; return *this; }

        [[nodiscard]] bool has(D2Capability c) const noexcept
        { return (caps_ & static_cast<std::uint32_t>(c)) != 0u; }
        [[nodiscard]] bool empty() const noexcept { return caps_ == 0u; }
        [[nodiscard]] std::uint32_t capabilities() const noexcept { return caps_; }

        [[nodiscard]] const FixedStepConfig& fixedStep() const noexcept { return fixed_step_; }
        [[nodiscard]] const Physics2DConfig& physicsConfig() const noexcept { return physics_; }
        [[nodiscard]] const PixelSimConfig&  pixelConfig() const noexcept { return pixel_; }

        /// True when any fixed-step capability is enabled — i.e. the scene needs a
        /// Simulation2DSystem. (A Sprite-only / physics-less scene installs none.)
        [[nodiscard]] bool needsSimulation() const noexcept
        { return has(D2Capability::Physics) || has(D2Capability::PixelSimulation); }

        /// Check capability dependencies + fixed-step bounds. Returns EVERY violation
        /// (bit flags) so an illegal combination is rejected wholesale — install() must
        /// refuse a plan that does not `validate().ok()`, never install it partially.
        /// Deterministic + allocation-free; the runtime-presence check (PixelSimulation
        /// needs a non-null PixelFieldRuntime) is an install()-time concern, not here.
        [[nodiscard]] D2PlanValidation validate() const noexcept
        {
            D2PlanValidation v;
            const auto flag = [&](D2PlanError e) { v.errors |= static_cast<std::uint32_t>(e); };

            // Core (Transform2D + Camera2D) is the L1 base: every optional layer acts on
            // Transform2D-placed entities, so a non-empty plan must enable it.
            if (!empty() && !has(D2Capability::Core))
                flag(D2PlanError::MissingCore);

            // PixelInterop is the pixel↔rigid seam — it needs BOTH sides present.
            if (has(D2Capability::PixelInterop))
            {
                if (!has(D2Capability::Physics))         flag(D2PlanError::PixelInteropRequiresPhysics);
                if (!has(D2Capability::PixelSimulation)) flag(D2PlanError::PixelInteropRequiresPixelSimulation);
            }

            // The character controller is a kinematic sweep over the collision world.
            if (has(D2Capability::CharacterController) && !has(D2Capability::Physics))
                flag(D2PlanError::CharacterControllerRequiresPhysics);

            // Fixed-step bounds only matter when a fixed-step capability actually runs.
            if (needsSimulation())
            {
                if (!(fixed_step_.fixed_dt > 0.0f))                        flag(D2PlanError::FixedStepInvalidDt);
                if (fixed_step_.max_substeps < 1)                         flag(D2PlanError::FixedStepInvalidSubsteps);
                if (fixed_step_.max_accumulated < fixed_step_.fixed_dt)   flag(D2PlanError::FixedStepInvalidAccumulated);
            }
            return v;
        }

    private:
        void set(D2Capability c) noexcept { caps_ |= static_cast<std::uint32_t>(c); }

        std::uint32_t   caps_{0u};
        FixedStepConfig fixed_step_{};
        Physics2DConfig physics_{};
        PixelSimConfig  pixel_{};
    };

} // namespace lux::gameplay::d2
