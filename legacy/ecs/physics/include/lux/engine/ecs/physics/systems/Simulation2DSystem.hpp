#pragma once
// ============================================================================
//  Simulation2DSystem.hpp — the unified fixed-step coordinator (lux::ecs).
//
//  A plain ISystem — it does NOT touch World (the fixed-step gateway is a kit
//  policy, not an engine one; design §2.4). It owns ONE accumulator so every
//  fixed-step capability (pixel sim, physics) shares a single timeline and can
//  never phase-drift. It implements NO physics or CA itself: it runs a fixed set
//  of PHASES in canonical order each fixed substep, and each phase is an OPTIONAL
//  injected strategy. Phase backing arrives per capability slice (F2 wires
//  SimulateFields, P2 wires SimulatePhysics, ...); install() REFUSES a plan that
//  enables a fixed-step capability no installed contribution has backed yet
//  backing table), so an enabled-but-dead coordinator cannot be installed.
//
//  Key contracts (design §2.4 + D-03):
//    - clamp banked time (max_accumulated) + cap substeps/frame (max_substeps) to
//      avoid a spiral of death; on hitting the cap, DISCARD the backlog and record it.
//    - never gate on a field view being empty — a physics-only scene still steps.
//      The loop runs whenever ANY phase is wired; an unwired phase is simply skipped.
// ============================================================================

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/physics/FixedStepConfig.hpp>
#include <lux/engine/function/visibility.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace lux::ecs
{
    class Simulation2DSystem final : public lux::ecs::ISystem
    {
    public:
        /// The ordered fixed-step phases (design §5.4). Single-direction flow: field
        /// commands → field sim → derived views → field→entity → physics → entity→field
        /// → events. An unwired phase is skipped, so Pixel-only, Physics-only and full
        /// interop are all just different subsets of the same ordered pipeline.
        enum class Phase : std::uint8_t
        {
            ApplyFieldCommands = 0,  ///< drain per-field command buffers into the field
            SimulateFields,          ///< CA-step the active chunks
            BuildDerived,            ///< derived views (pressure/heat) later phases read
            FieldToEntity,           ///< field → entity (sample materials, spawn from field)
            SimulatePhysics,         ///< collision + rigid/character integration
            CollectEntityToField,    ///< entity → field (rasterize bodies back)
            PublishEvents,           ///< flush this-step events to consumers
            Count
        };
        static constexpr std::size_t kPhaseCount = static_cast<std::size_t>(Phase::Count);

        /// A phase strategy: given the registry + the FIXED timestep, advance one substep.
        using PhaseFn = std::function<void(lux::ecs::Registry&, float /*fixed_dt*/)>;

        explicit Simulation2DSystem(const FixedStepConfig& cfg) noexcept : cfg_(cfg) {}

        /// Wire (or replace) a phase's strategy — install-time only. An empty fn clears it.
        void setPhase(Phase p, PhaseFn fn) { phases_[static_cast<std::size_t>(p)] = std::move(fn); }
        [[nodiscard]] bool hasPhase(Phase p) const noexcept
        { return static_cast<bool>(phases_[static_cast<std::size_t>(p)]); }

        LUX_FUNCTION_PUBLIC void update(const lux::ecs::SystemUpdateContext& ctx) override;

        [[nodiscard]] float         accumulated()       const noexcept { return accumulator_; }
        [[nodiscard]] const FixedStepConfig& config()   const noexcept { return cfg_; }

    private:
        [[nodiscard]] bool hasAnyPhase() const noexcept;

        FixedStepConfig                  cfg_;
        std::array<PhaseFn, kPhaseCount> phases_{};
        float         accumulator_          = 0.0f;
        int           substeps_last_frame_  = 0;   ///< 帧内步进上限的循环计数(逻辑用,非观测)
    };

} // namespace lux::ecs
