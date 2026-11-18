#pragma once
#include <cstdint>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/extensions/features/EffectDefinition.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace lux::render
{
    class FrameData;
    // SDK values. Never expose Storage/operation-state/native pointers to callers.
    enum class EJobCadence : std::uint8_t
    {
        CONTINUOUS,
        REQUESTED
    };

    [[nodiscard]] constexpr bool isValidJobCadence(EJobCadence cadence) noexcept
    {
        return cadence == EJobCadence::CONTINUOUS || cadence == EJobCadence::REQUESTED;
    }

    struct JobCreated final
    {
        RenderError error{};
        RenderJobId job{};
    };

    struct JobSubmitted final
    {
        RenderError error{};
        RenderJobId job{};
        std::uint64_t frame_index = 0;
        std::uint32_t slot_index = 0;
    };

    struct TargetImageInput final
    {
        FeaturePortRef destination; // External input port declared by the Effect author.
        TargetId target;
    };

    struct JobInputs final
    {
        std::vector<std::byte> parameters; // Execution-local parameter region.
        RasterViewport viewport;
        std::vector<std::uint8_t> conditions;
    };

    struct JobBinding final
    {
        JobInputs inputs;
        std::vector<TargetImageInput> target_images;
    };

    struct CreateRenderJob final
    {
        static constexpr std::string_view name = "render.job.create";

        struct Request final
        {
            SceneId scene;
            ViewId view;
            EffectId effect;
            EJobCadence cadence = EJobCadence::CONTINUOUS;
        };

        using Response = JobCreated;
    };

    struct StartRenderJob final
    {
        static constexpr std::string_view name = "render.job.start";
        // Handler prepares asynchronously and publishes at a LOCAL safe point.
        // Do not let the old global frame-boundary flag stall all Devices.

        struct Request final
        {
            RenderJobId job;
            TargetId target;
            JobBinding binding;
        };

        using Response = AckResponse;
    };

    struct StopRenderJob final
    {
        static constexpr std::string_view name = "render.job.stop";
        static constexpr bool failure_safe = true;
        using Request = RenderJobId;
        using Response = AckResponse; // no future admission; NOT a GPU completion
    };

    struct RemoveRenderJob final
    {
        static constexpr std::string_view name = "render.job.remove";
        static constexpr bool failure_safe = true;
        using Request = RenderJobId;
        using Response = AckResponse; // Busy while any lifecycle/CPU borrow exists
    };

    struct SetRenderJobDependencies final
    {
        static constexpr std::string_view name = "render.job.dependencies.set";

        struct Request final
        {
            RenderJobId job;
            // Owning input: async IPC does not borrow a caller's temporary span.
            std::vector<RenderJobId> predecessors;
            std::vector<RenderJobId> successors;
        };

        using Response = AckResponse;
    };

    struct SetRenderJobCadence final
    {
        static constexpr std::string_view name = "render.job.cadence.set";

        struct Request final
        {
            RenderJobId job;
            EJobCadence cadence;
        };

        using Response = AckResponse;
    };

    struct RequestRenderJob final
    {
        static constexpr std::string_view name = "render.job.request";
        struct Request final
        {
            RenderJobId job;
            FrameData* data = nullptr;
        };
        // Exactly one pending exact request per Job. Completion releases the optional
        // input borrow. Success identifies native submission acceptance.
        using Response = JobSubmitted;
        static FrameData* frameData(const Request& request) noexcept { return request.data; }
    };

    struct RebindRenderJobEffect final
    {
        static constexpr std::string_view name = "render.job.effect.rebind";

        struct Request final
        {
            RenderJobId job;
            EffectId effect;
            TargetId target; // Null retains the running output target.
            std::optional<JobBinding> binding; // Omitted retains the installed binding.
        };

        using Response = AckResponse;
    };

    struct UpdateJobInputs final
    {
        static constexpr std::string_view name = "render.job.inputs";
        struct Request final
        {
            RenderJobId job;
            JobInputs inputs;
        };
        using Response = AckResponse;
    };

    struct InvalidateHistory final
    {
        static constexpr std::string_view name = "render.job.invalidate_history";
        struct Request final
        {
            RenderJobId job;
            std::vector<GraphResourceId> previous;
        };
        using Response = AckResponse;
    };

    struct QueryExecutionProgress final
    {
        static constexpr std::string_view name = "render.execution.progress";
        static constexpr bool failure_safe = true;
        using Request = RenderJobId;
        struct Response final
        {
            RenderError error{};
            ExecutionProgress progress{};
            RenderError last_run_error{};
        };
    };

} // namespace lux::render
