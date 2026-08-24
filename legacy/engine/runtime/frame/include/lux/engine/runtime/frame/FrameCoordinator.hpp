#pragma once
/**
 * @file FrameCoordinator.hpp
 * @brief The single main-thread protocol for opening, advancing, and
 *        submitting an engine frame.
 *
 * A host may insert work at three named points, but it never manipulates the
 * RenderFrameSession frame state directly:
 *
 *   previous replies/submit -> open -> beforeMain -> main completions
 *   -> beforeEvents -> frame events -> record -> submit
 *
 * Frame is a move-only lexical transaction.  record() is the normal closing
 * path.  Its destructor performs a non-blocking submit only as a channel-state
 * safety net; it never runs user callbacks, drains tasks, or waits.
 *
 * Main-thread confined by contract.  No lock protects the protocol state;
 * ownership of the coordinator is the synchronization mechanism.
 */

#include <lux/engine/runtime/frame/visibility.h>
#include <lux/engine/runtime/execution/AsyncStatistics.hpp>

#include <cassert>
#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lux::events
{
    class EventPump;
}

namespace lux::exec
{
    class AsyncRuntime;
}

namespace lux::render
{
    class RenderFrameSession;
    class RenderControlSession;
}

namespace lux::runtime
{
    class FrameCoordinator;

    enum class EFrameTracePhase : std::uint8_t
    {
        CONTROL_REPLY_PUMP,
        FRAME_REPLY_PUMP,
        PENDING_SUBMIT,
        FRAME_SLOT_WAIT,
        FRAME_OPEN,
        MAIN_COMPLETION,
        EVENT_DRAIN,
        TEXTURE_STREAMING,
        SCHEDULE_INPUT,
        SCHEDULE_PRE_TRANSFORM,
        SCHEDULE_SIMULATION,
        SCHEDULE_PRE_RENDER,
        SCHEDULE_RENDER,
        SCHEDULE_POST_RENDER,
        COMMAND_BARRIER,
        FRAME_SUBMIT,
        COUNT
    };

    inline constexpr std::size_t kFrameTracePhaseCount =
        static_cast<std::size_t>(EFrameTracePhase::COUNT);

    struct FrameTrace final
    {
        std::uint64_t frame_serial{0u};
        std::uint64_t wall_nanoseconds{0u};
        std::array<std::uint64_t, kFrameTracePhaseCount>
            phase_nanoseconds{};
        std::uint64_t allocation_count{0u};
        std::uint64_t allocation_bytes{0u};

        [[nodiscard]] std::uint64_t attributedNanoseconds() const noexcept
        {
            std::uint64_t result = 0u;
            for (const auto value : phase_nanoseconds)
                result += value;
            return result;
        }
    };

    class LUX_FRAME_RUNTIME_PUBLIC Frame final
    {
    public:
        Frame() noexcept = default;
        ~Frame() noexcept;

        Frame(const Frame&)            = delete;
        Frame& operator=(const Frame&) = delete;

        Frame(Frame&& other) noexcept;
        Frame& operator=(Frame&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return owner_ != nullptr;
        }

        [[nodiscard]] std::uint64_t sequence() const noexcept
        {
            return sequence_;
        }

        void recordPhase(
            EFrameTracePhase phase,
            std::uint64_t nanoseconds) noexcept;

        void recordAllocations(
            std::uint64_t count,
            std::uint64_t bytes) noexcept;

        /**
         * Run host work that must happen immediately after the render frame
         * opens, then establish the main-thread completion safe point.
         */
        template <std::invocable Fn>
        void beforeMain(Fn&& fn)
        {
            assert(owner_ && "beforeMain() requires an open frame");
            assert(stage_ == Stage::Open && "beforeMain() must be the first frame phase");
            if (!owner_ || stage_ != Stage::Open)
                return;

            std::invoke(std::forward<Fn>(fn));
            drainMainThreadCompletions();
        }

        /**
         * Run event-source work after main completions are visible, then
         * drain the frame event pump exactly once.
         */
        template <std::invocable Fn>
        void beforeEvents(Fn&& fn)
        {
            assert(owner_ && "beforeEvents() requires an open frame");
            if (!owner_)
                return;

            drainMainThreadCompletions();
            assert(stage_ == Stage::MainDrained);
            std::invoke(std::forward<Fn>(fn));
            drainEvents();
        }

        /**
         * Establish any omitted safe points, record the caller's frame work,
         * and submit.  This is the normal terminal operation for a Frame.
         */
        template <std::invocable Fn>
        void record(Fn&& fn)
        {
            assert(owner_ && "record() requires an open frame");
            if (!owner_)
                return;

            drainEvents();
            std::invoke(std::forward<Fn>(fn));
            finish(false);
        }

    private:
        friend class FrameCoordinator;

        enum class Stage : std::uint8_t
        {
            Open,
            MainDrained,
            EventsDrained
        };

        explicit Frame(FrameCoordinator& owner, std::uint64_t sequence) noexcept;

        void drainMainThreadCompletions();
        void drainEvents();
        void finish(bool fallback) noexcept;

        FrameCoordinator* owner_{nullptr}; // lexical borrow; never crosses a frame
        std::uint64_t      sequence_{0};
        Stage              stage_{Stage::Open};
    };

    class LUX_FRAME_RUNTIME_PUBLIC FrameCoordinator final
    {
    public:
        struct Statistics
        {
            std::uint64_t opened{0};
            std::uint64_t start_failures{0};
            std::uint64_t submitted{0};
            std::uint64_t submit_failures{0};
            std::uint64_t fallback_submit_attempts{0};
            std::uint64_t frame_slot_wait_ns{0};
            std::uint64_t frame_slot_wait_samples{0};
            std::uint64_t frame_slot_wait_max_ns{0};
            lux::exec::AsyncLatencyHistogram frame_slot_wait_histogram{};
            std::uint64_t main_completions_while_waiting{0};
        };

        FrameCoordinator(lux::render::RenderFrameSession& session,
                         lux::events::EventPump&     events) noexcept;
        FrameCoordinator(lux::render::RenderFrameSession& session,
                         lux::events::EventPump&     events,
                         lux::exec::AsyncRuntime&    async) noexcept;
        FrameCoordinator(lux::render::RenderFrameSession&        session,
                         lux::render::RenderControlSession& control,
                         lux::events::EventPump&             events,
                         lux::exec::AsyncRuntime&            async) noexcept;
        ~FrameCoordinator();

        FrameCoordinator(const FrameCoordinator&)            = delete;
        FrameCoordinator& operator=(const FrameCoordinator&) = delete;
        FrameCoordinator(FrameCoordinator&&)                 = delete;
        FrameCoordinator& operator=(FrameCoordinator&&)      = delete;

        /** Flush the previous frame and open the next one. */
        [[nodiscard]] Frame begin();

        /// Advance replies, MainThreadMailbox adoption and domain facts without
        /// opening a render frame. Minimized, headless-idle and close paths
        /// call this same safe point.
        std::size_t pumpSafePoint();
        /// Stop touching frame/control sessions while retaining the shared
        /// progress domain, MainThreadMailbox adoption and DomainEvents drain.
        /// The composition root calls this immediately before the render host
        /// destroys its sessions; post-render LogRouter and AsyncRuntime close
        /// still use the same MainCloseDriver without dangling borrows.
        void detachRenderSessions() noexcept;
        [[nodiscard]] std::uint64_t observeProgress() const noexcept;
        void waitForProgress(std::uint64_t observed) const noexcept;
        [[nodiscard]] bool waitForProgressUntil(
            std::uint64_t observed,
            std::chrono::steady_clock::time_point deadline) const noexcept;
        void notifyProgress() noexcept;

        [[nodiscard]] const Statistics& statistics() const noexcept
        {
            return statistics_;
        }

        [[nodiscard]] std::optional<FrameTrace> latestTrace() const noexcept;
        [[nodiscard]] std::vector<FrameTrace> traceHistory() const;

        [[nodiscard]] bool frameActive() const noexcept
        {
            return frame_active_;
        }

    private:
        friend class Frame;

        std::size_t drainMainThreadCompletions();
        void drainEvents();
        void submit(bool fallback) noexcept;
        [[nodiscard]] bool submitPendingFrame();
        void recordPhase(
            EFrameTracePhase phase,
            std::uint64_t nanoseconds) noexcept;
        void commitActiveTrace() noexcept;

        struct Impl;

        std::reference_wrapper<lux::render::RenderFrameSession> session_;
        std::reference_wrapper<lux::events::EventPump>     events_;
        std::optional<std::reference_wrapper<lux::exec::AsyncRuntime>> async_;
        std::optional<std::reference_wrapper<lux::render::RenderControlSession>> control_;
        Statistics  statistics_{};
        static constexpr std::size_t kTraceCapacity = 4096u;
        FrameTrace active_trace_{};
        std::chrono::steady_clock::time_point active_trace_started_{};
        std::uint64_t next_sequence_{1};
        bool          trace_recording_{false};
        bool          frame_active_{false};
        bool          render_attached_{true};
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::runtime
