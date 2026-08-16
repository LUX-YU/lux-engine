// ============================================================================
//  Simulation2DSystem.cpp — the fixed-step accumulator loop (design §2.4).
// ============================================================================

#include <lux/engine/ecs/physics/systems/Simulation2DSystem.hpp>

#include <algorithm>   // std::min
#include <cmath>       // std::fmod

namespace lux::ecs
{
    bool Simulation2DSystem::hasAnyPhase() const noexcept
    {
        for (const auto& fn : phases_)
            if (fn) return true;
        return false;
    }

    void Simulation2DSystem::update(const lux::ecs::SystemUpdateContext& ctx)
    {
        auto& registry = ctx.registry();
        const float dt = ctx.dt();
        substeps_last_frame_ = 0;

        // Physics-agnostic gate: run whenever ANY phase is wired — NOT gated on a field
        // view being empty, so a physics-only scene still steps (D-03). Zero cost when
        // no fixed-step capability is installed.
        if (!hasAnyPhase())
            return;

        // Clamp banked time so a long stall (breakpoint, load hitch) can't schedule a
        // burst of catch-up substeps. The clamp is an unconditional hard bound (documented
        // on FixedStepConfig::max_accumulated; drop_excess_time only governs the
        // substep-cap case below) — what it discards is DISCARDED BACKLOG by policy.
        accumulator_ = std::min(accumulator_ + dt, cfg_.max_accumulated);

        while (accumulator_ >= cfg_.fixed_dt && substeps_last_frame_ < cfg_.max_substeps)
        {
            for (std::size_t i = 0; i < kPhaseCount; ++i)
                if (phases_[i])
                    phases_[i](registry, cfg_.fixed_dt);   // canonical order (enum order)
            accumulator_ -= cfg_.fixed_dt;
            ++substeps_last_frame_;
        }

        // Hit the substep cap with time still banked → discard the backlog (explicit lag
        // policy), so we never run an ever-growing debt.
        if (substeps_last_frame_ >= cfg_.max_substeps && accumulator_ >= cfg_.fixed_dt)
        {
            accumulator_ = cfg_.drop_excess_time
                               ? std::fmod(accumulator_, cfg_.fixed_dt)
                               : accumulator_;
        }
    }

} // namespace lux::ecs
