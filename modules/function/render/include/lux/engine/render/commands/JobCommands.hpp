#pragma once
#include <cstdint>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <string_view>
#include <vector>

namespace lux::render
{
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

    struct CreateRenderJob final
    {
        static constexpr std::string_view name = "render.job.create";
        static constexpr bool requires_frame_boundary = false;

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
        static constexpr bool requires_frame_boundary = false;

        struct Request final
        {
            RenderJobId job;
            TargetId target;
        };

        using Response = AckResponse;
    };

    struct StopRenderJob final
    {
        static constexpr std::string_view name = "render.job.stop";
        static constexpr bool requires_frame_boundary = false;
        static constexpr bool failure_safe = true;
        using Request = RenderJobId;
        using Response = AckResponse; // no future admission; NOT a GPU completion
    };

    struct RemoveRenderJob final
    {
        static constexpr std::string_view name = "render.job.remove";
        static constexpr bool requires_frame_boundary = false;
        static constexpr bool failure_safe = true;
        using Request = RenderJobId;
        using Response = AckResponse; // Busy while any lifecycle/CPU borrow exists
    };

    struct SetRenderJobDependencies final
    {
        static constexpr std::string_view name = "render.job.dependencies.set";
        static constexpr bool requires_frame_boundary = false;

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
        static constexpr bool requires_frame_boundary = false;

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
        static constexpr bool requires_frame_boundary = false;
        using Request = RenderJobId;
        // Exactly one pending exact request per Job. Busy is explicit. A successful
        // reply identifies actual native submission, not enqueuing and not display.
        using Response = JobSubmitted;
    };

    struct RebindRenderJobEffect final
    {
        static constexpr std::string_view name = "render.job.effect.rebind";
        static constexpr bool requires_frame_boundary = false;

        struct Request final
        {
            RenderJobId job;
            EffectId effect;
        };

        using Response = AckResponse;
    };

} // namespace lux::render
