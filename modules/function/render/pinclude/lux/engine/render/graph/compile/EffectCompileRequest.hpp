#pragma once

#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/effect/EffectGraphBuild.hpp>
#include <atomic>

namespace lux::render::detail
{
    struct EffectCompileRequest final
    {
        runtime_detail::EffectRecipeView recipe;
        std::vector<runtime_detail::EffectInputDefinition> inputs;
        GraphOutputRequirements output;
        bool uploads_available;
        std::optional<EGraphUse> target_final_use;
    };

    struct EffectCompileEnvironment final
    {
        const EffectCatalog& catalog;
        execution::CpuScheduler cpu;
        RenderErrorSink diagnostics;
        const std::atomic<bool>& stopping;
        const RenderError& device_failure;
    };
} // namespace lux::render::detail
