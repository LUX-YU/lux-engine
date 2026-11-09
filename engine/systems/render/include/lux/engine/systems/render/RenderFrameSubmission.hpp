#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/systems/render/visibility.h>
#include <exec/task.hpp>

#include <cstdint>
#include <memory>

namespace lux::render
{
    class FrameStorage;
    class FrameWriter;
    class RenderClient;
}

namespace lux::engine::render
{
    // One application-thread frame stream shared by all RenderSystems. EngineContext owns it;
    // Engine awaits beginFrame before Variable/SceneSystem updates, then submits or discards once.
    // Fixed groups may advance while admission waits; they do not write this frame stream.
    class LUX_ENGINE_SYSTEM_RENDER_PUBLIC RenderFrameSubmission final
    {
    public:
        using Result = lux::render::RenderResult<void>;

        // EngineContext only, after RenderClient. Matches RenderServerConfig::max_frames_ahead:
        // one frame being consumed plus this many prepared/queued frames. UINT32_MAX is invalid.
        RenderFrameSubmission(lux::render::RenderClient& client, std::uint32_t max_frames_ahead);
        // Destroy outside Execution dispatch, after the driver and all frame writers have ended.
        // Settles submitted borrows while RenderClient, Server and Execution are still alive.
        ~RenderFrameSubmission() noexcept;
        RenderFrameSubmission(const RenderFrameSubmission&) = delete;
        RenderFrameSubmission& operator=(const RenderFrameSubmission&) = delete;

        [[nodiscard]] static RenderFrameSubmission& instance() noexcept;

        // Start on Main. At most one begin may be pending. Waiting suspends the task, not Main;
        // replies resume it through Execution. End the driver before destroying this object.
        [[nodiscard]] exec::task<Result> beginFrame() noexcept;

        // One serial owner wait. Captures existing submissions; later frames do not extend it.
        // Completion proves CPU input borrows ended, not GPU idleness.
        [[nodiscard]] exec::task<Result> whenSubmittedFramesConsumed() noexcept;

        // Borrowed writer for the current admitted frame; never retain it across submission.
        [[nodiscard]] lux::render::FrameWriter writer() noexcept;
        // Read-only packet inspection; publication remains exclusively owned by this stream.
        [[nodiscard]] const lux::render::FrameStorage& frame() const noexcept;

        // Starts reply handling in the existing Execution scope; does not wait for this frame.
        // All outcomes reach observers. An unsubmitted busy frame or a stale-frame rejection
        // requests fresh collection; other asynchronous failures reach the next beginFrame result.
        [[nodiscard]] Result submitFrame() noexcept;

        // No-op if this round has no unsubmitted frame; never rolls back submitted work.
        void discardFrame() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::engine::render
