#pragma once

#include <lux/engine/render/runtime/JobControl.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/runtime/JobRelations.hpp>
#include <memory>

namespace lux::render::runtime_detail
{
    struct RenderJobContent final
    {
        SceneId scene;
        ViewId view;
        EffectId effect;
    };

    struct RenderJob final
    {
        RenderJob(RenderJobContent content, EJobCadence cadence) noexcept : content(content), control(cadence) {}

        RenderJobContent content;
        detail::JobControl control;
        detail::JobRelations relations;
        JobBinding binding;
        std::unique_ptr<JobExecution> execution;
        RenderError last_run_error;

        [[nodiscard]] bool readyToRun() const noexcept
        {
            return control.eligible() && (!last_run_error.type || control.hasRequest());
        }

        [[nodiscard]] bool hasExecutionBorrows() const noexcept
        {
            return control.hasRun() || (execution && execution->pending_uses != 0);
        }
    };
} // namespace lux::render::runtime_detail
