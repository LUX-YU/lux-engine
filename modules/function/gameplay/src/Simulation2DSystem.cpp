// ============================================================================
//  Simulation2DSystem.cpp — the fixed-step accumulator loop (design §2.4).
// ============================================================================

#include <lux/engine/gameplay/2d/world/systems/Simulation2DSystem.hpp>

#include <algorithm>   // std::min
#include <cmath>       // std::fmod

namespace lux::gameplay::d2
{
    bool Simulation2DSystem::hasAnyPhase() const noexcept
    {
        for (const auto& fn : phases_)
            if (fn) return true;
        return false;
    }

    void Simulation2DSystem::update(lux::meta::EntityRegistry& registry, float dt)
    {
        substeps_last_frame_ = 0;

        // Physics-agnostic gate: run whenever ANY phase is wired — NOT gated on a field
        // view being empty, so a physics-only scene still steps (D-03). Zero cost when
        // no fixed-step capability is installed.
        if (!hasAnyPhase())
            return;

        // Clamp banked time so a long stall (breakpoint, load hitch) can't schedule a
        // burst of catch-up substeps. The clamp is an unconditional hard bound (documented
        // on FixedStepConfig::max_accumulated; drop_excess_time only governs the
        // substep-cap case below) — but what it discards is still DISCARDED BACKLOG, so
        // it must be booked into lagged_time_ like every other drop: laggedTime() is
        // "total discarded backlog", and a 1s stall that banks only 0.25s dropped 0.75s.
        const float banked = accumulator_ + dt;
        accumulator_ = std::min(banked, cfg_.max_accumulated);
        lagged_time_ += banked - accumulator_;

        while (accumulator_ >= cfg_.fixed_dt && substeps_last_frame_ < cfg_.max_substeps)
        {
            for (std::size_t i = 0; i < kPhaseCount; ++i)
                if (phases_[i])
                    phases_[i](registry, cfg_.fixed_dt);   // canonical order (enum order)
            accumulator_ -= cfg_.fixed_dt;
            ++substeps_last_frame_;
            ++total_substeps_;
        }

        // Hit the substep cap with time still banked → discard the backlog (explicit lag
        // policy), recording how much we dropped, so we never run an ever-growing debt.
        if (substeps_last_frame_ >= cfg_.max_substeps && accumulator_ >= cfg_.fixed_dt)
        {
            const float kept = cfg_.drop_excess_time
                                   ? std::fmod(accumulator_, cfg_.fixed_dt)
                                   : accumulator_;
            lagged_time_ += (accumulator_ - kept);
            accumulator_  = kept;
        }
    }

} // namespace lux::gameplay::d2
