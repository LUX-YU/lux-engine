#pragma once

namespace lux::render::runtime_detail
{
    struct RenderEffect;

    class EffectExecutionUse final
    {
    public:
        EffectExecutionUse(const EffectExecutionUse&) = delete;
        EffectExecutionUse& operator=(const EffectExecutionUse&) = delete;
        EffectExecutionUse(EffectExecutionUse&&) noexcept;
        EffectExecutionUse& operator=(EffectExecutionUse&&) noexcept;
        ~EffectExecutionUse() noexcept;

    private:
        friend struct RenderEffect;
        explicit EffectExecutionUse(RenderEffect& effect) noexcept : effect_(&effect) {}
        RenderEffect* effect_;
    };
} // namespace lux::render::runtime_detail
