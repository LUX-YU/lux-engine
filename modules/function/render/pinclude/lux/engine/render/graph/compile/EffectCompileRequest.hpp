#pragma once

#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/render/effect/EffectGraphBuild.hpp>
#include <atomic>

namespace lux::render::detail
{
    struct EffectCompileRequest final
    {
        EffectId effect;
        runtime_detail::EffectRecipeView recipe;
        std::vector<runtime_detail::EffectInputDefinition> inputs;
        EffectOutputDesc output;
        const runtime_detail::EffectImportContracts& imports;
    };

    struct EffectCompileEnvironment final
    {
        const EffectCatalog& catalog;
        execution::CpuScheduler cpu;
        RenderErrorSink diagnostics;
        const std::atomic<bool>& stopping;
    };
} // namespace lux::render::detail
