#include <lux/engine/runtime/frame/FrameCoordinator.hpp>

#include <lux/engine/events/DomainEvents.hpp>
#include <lux/cxx/concurrent/AtomicWait.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/detail/MainThreadMailbox.hpp>
#include <lux/engine/function/render/client/FrameProgram.hpp>
#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>

#include <algorithm>
#include <utility>
#include <chrono>

namespace lux::runtime
{
    namespace
    {
        [[nodiscard]] std::uint64_t elapsedNanoseconds(
            std::chrono::steady_clock::time_point started) noexcept
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count());
        }
    }

    struct FrameCoordinator::Impl final
    {
        lux::exec::AsyncRuntime* runtime{nullptr};
        std::shared_ptr<lux::render::RenderChannelSync> progress;
        std::shared_ptr<const lux::exec::MainThreadMailbox::WakeBinding> wake;
        std::array<FrameTrace, FrameCoordinator::kTraceCapacity> traces{};
        std::size_t trace_begin{0u};
        std::size_t trace_size{0u};
    };

    Frame::Frame(FrameCoordinator& owner, std::uint64_t sequence) noexcept
        : owner_(&owner), sequence_(sequence)
    {
    }

    Frame::~Frame() noexcept
    {
        finish(true);
    }

    Frame::Frame(Frame&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)),
          sequence_(std::exchange(other.sequence_, 0)),
          stage_(other.stage_)
    {
    }

    Frame& Frame::operator=(Frame&& other) noexcept
    {
        if (this == &other)
            return *this;

        finish(true);
        owner_    = std::exchange(other.owner_, nullptr);
        sequence_ = std::exchange(other.sequence_, 0);
        stage_    = other.stage_;
        return *this;
    }

    void Frame::recordPhase(
        EFrameTracePhase phase,
        std::uint64_t nanoseconds) noexcept
    {
        if (owner_)
            owner_->recordPhase(phase, nanoseconds);
    }

    void Frame::recordAllocations(
        std::uint64_t count,
        std::uint64_t bytes) noexcept
    {
        if (!owner_)
            return;
        owner_->active_trace_.allocation_count += count;
        owner_->active_trace_.allocation_bytes += bytes;
    }

    void Frame::drainMainThreadCompletions()
    {
        if (!owner_ || stage_ != Stage::Open)
            return;
        owner_->drainMainThreadCompletions();
        stage_ = Stage::MainDrained;
    }

    void Frame::drainEvents()
    {
        if (!owner_)
            return;
        drainMainThreadCompletions();
        if (stage_ != Stage::MainDrained)
            return;
        owner_->drainEvents();
        stage_ = Stage::EventsDrained;
    }

    void Frame::finish(bool fallback) noexcept
    {
        if (!owner_)
            return;
        auto* owner = std::exchange(owner_, nullptr);
        owner->submit(fallback);
    }

    FrameCoordinator::FrameCoordinator(lux::render::RenderFrameSession& session,
                                       lux::events::EventPump& events) noexcept
        : session_(session), events_(events), impl_(std::make_unique<Impl>())
    {
        impl_->progress = session.progressDomain();
    }

    FrameCoordinator::FrameCoordinator(
        lux::render::RenderFrameSession& session,
        lux::render::RenderControlSession& control,
        lux::events::EventPump& events,
        lux::exec::AsyncRuntime& async) noexcept
        : session_(session), events_(events), async_(async),
          control_(control), impl_(std::make_unique<Impl>())
    {
        impl_->runtime = &async;
        impl_->progress = session.progressDomain();
        impl_->wake = std::make_shared<lux::exec::MainThreadMailbox::WakeBinding>(
            lux::exec::MainThreadMailbox::WakeBinding{
                session.progressDomain(),
                [](void* context) noexcept
                {
                    static_cast<lux::render::RenderChannelSync*>(context)
                        ->notifyRequestStateChanged();
                }});
        async.mainThreadMailbox().bindExternalWake(impl_->wake);
    }

    FrameCoordinator::FrameCoordinator(lux::render::RenderFrameSession& session,
                                       lux::events::EventPump& events,
                                       lux::exec::AsyncRuntime& async) noexcept
        : session_(session), events_(events), async_(async),
          impl_(std::make_unique<Impl>())
    {
        impl_->runtime = &async;
        impl_->progress = session.progressDomain();
        impl_->wake = std::make_shared<lux::exec::MainThreadMailbox::WakeBinding>(
            lux::exec::MainThreadMailbox::WakeBinding{
                session.progressDomain(),
                [](void* context) noexcept
                {
                    static_cast<lux::render::RenderChannelSync*>(context)
                        ->notifyRequestStateChanged();
                }});
        async.mainThreadMailbox().bindExternalWake(impl_->wake);
    }

    FrameCoordinator::~FrameCoordinator()
    {
        if (impl_ && impl_->runtime && impl_->wake)
            impl_->runtime->mainThreadMailbox().unbindExternalWake(impl_->wake);
    }

    Frame FrameCoordinator::begin()
    {
        if (frame_active_ || !render_attached_)
        {
            ++statistics_.start_failures;
            return {};
        }

        active_trace_ = {};
        active_trace_.frame_serial = next_sequence_;
        active_trace_started_ = std::chrono::steady_clock::now();
        trace_recording_ = true;

        auto& session = session_.get();
        // Normal lexical frames preserve their named phases: replies may be
        // adopted before opening, but MainThreadMailbox and DomainEvents are drained
        // by Frame::beforeMain()/beforeEvents(). pumpSafePoint() is the
        // explicit idle/minimized path and must not consume frame facts early.
        if (control_)
        {
            const auto started = std::chrono::steady_clock::now();
            (void)control_->get().pumpReplies();
            recordPhase(
                EFrameTracePhase::CONTROL_REPLY_PUMP,
                elapsedNanoseconds(started));
        }
        {
            const auto started = std::chrono::steady_clock::now();
            (void)session.pumpReplies();
            recordPhase(
                EFrameTracePhase::FRAME_REPLY_PUMP,
                elapsedNanoseconds(started));
        }
        // submitPendingFrame() records the waits and safe-point work it performs
        // while backpressured.  PENDING_SUBMIT is the exclusive remainder so a
        // benchmark may sum every phase without counting a slot wait twice.
        const auto nestedPendingNanoseconds = [this]() noexcept
        {
            constexpr std::array nested_phases{
                EFrameTracePhase::CONTROL_REPLY_PUMP,
                EFrameTracePhase::FRAME_REPLY_PUMP,
                EFrameTracePhase::FRAME_SLOT_WAIT,
                EFrameTracePhase::MAIN_COMPLETION};
            std::uint64_t result = 0u;
            for (const auto phase : nested_phases)
            {
                result += active_trace_.phase_nanoseconds[
                    static_cast<std::size_t>(phase)];
            }
            return result;
        };
        const auto nested_before = nestedPendingNanoseconds();
        const auto pending_started = std::chrono::steady_clock::now();
        const bool pending_submitted = submitPendingFrame();
        const auto pending_elapsed = elapsedNanoseconds(pending_started);
        const auto nested_elapsed = nestedPendingNanoseconds() - nested_before;
        recordPhase(
            EFrameTracePhase::PENDING_SUBMIT,
            pending_elapsed > nested_elapsed
                ? pending_elapsed - nested_elapsed
                : 0u);
        const auto open_started = std::chrono::steady_clock::now();
        const bool opened = pending_submitted && session.beginFrame({});
        recordPhase(
            EFrameTracePhase::FRAME_OPEN,
            elapsedNanoseconds(open_started));
        if (!opened)
        {
            trace_recording_ = false;
            ++statistics_.start_failures;
            return {};
        }

        ++statistics_.opened;
        frame_active_ = true;
        return Frame{*this, next_sequence_++};
    }

    std::size_t FrameCoordinator::pumpSafePoint()
    {
        std::size_t render_replies = 0u;
        if (render_attached_)
        {
            if (control_)
                render_replies += control_->get().pumpReplies();
            render_replies += session_.get().pumpReplies();
        }
        const auto ran = drainMainThreadCompletions();
        drainEvents();
        return render_replies + ran;
    }

    void FrameCoordinator::detachRenderSessions() noexcept
    {
        assert(!frame_active_ &&
            "render sessions cannot detach during an active frame");
        render_attached_ = false;
        control_.reset();
    }

    std::uint64_t FrameCoordinator::observeProgress() const noexcept
    {
        return impl_->progress->work_epoch.load(std::memory_order_acquire);
    }

    void FrameCoordinator::waitForProgress(std::uint64_t observed) const noexcept
    {
        impl_->progress->work_epoch.wait(
            observed, std::memory_order_acquire);
    }

    bool FrameCoordinator::waitForProgressUntil(
        std::uint64_t observed,
        std::chrono::steady_clock::time_point deadline) const noexcept
    {
        return lux::cxx::concurrent::waitAtomicU64Until(
            impl_->progress->work_epoch,
            observed,
            deadline);
    }

    void FrameCoordinator::notifyProgress() noexcept
    {
        impl_->progress->notifyRequestStateChanged();
    }

    std::size_t FrameCoordinator::drainMainThreadCompletions()
    {
        if (!async_)
            return 0u;
        const auto started = std::chrono::steady_clock::now();
        const auto result = async_->get().drainMainThreadCompletions();
        recordPhase(
            EFrameTracePhase::MAIN_COMPLETION,
            elapsedNanoseconds(started));
        return result;
    }

    void FrameCoordinator::drainEvents()
    {
        const auto started = std::chrono::steady_clock::now();
        events_.get().drain();
        recordPhase(
            EFrameTracePhase::EVENT_DRAIN,
            elapsedNanoseconds(started));
    }

    void FrameCoordinator::submit(bool fallback) noexcept
    {
        assert(frame_active_ && "submitting without an active frame");
        if (!frame_active_)
        {
            ++statistics_.submit_failures;
            return;
        }
        frame_active_ = false;
        if (fallback)
            ++statistics_.fallback_submit_attempts;
        const auto started = std::chrono::steady_clock::now();
        if (session_.get().trySubmitFrame())
            ++statistics_.submitted;
        else
            ++statistics_.submit_failures;
        recordPhase(
            EFrameTracePhase::FRAME_SUBMIT,
            elapsedNanoseconds(started));
        commitActiveTrace();
    }

    bool FrameCoordinator::submitPendingFrame()
    {
        auto& session = session_.get();
        for (;;)
        {
            if (session.trySubmitFrame())
                return true;
            if (session.isStopping())
                return false;

            const auto observed = session.observeProgress();
            if (control_)
            {
                const auto started = std::chrono::steady_clock::now();
                (void)control_->get().pumpReplies();
                recordPhase(
                    EFrameTracePhase::CONTROL_REPLY_PUMP,
                    elapsedNanoseconds(started));
            }
            {
                const auto started = std::chrono::steady_clock::now();
                (void)session.pumpReplies();
                recordPhase(
                    EFrameTracePhase::FRAME_REPLY_PUMP,
                    elapsedNanoseconds(started));
            }
            statistics_.main_completions_while_waiting += drainMainThreadCompletions();
            if (session.trySubmitFrame())
                return true;
            if (session.isStopping())
                return false;

            const auto wait_started = std::chrono::steady_clock::now();
            session.waitForProgress(observed);
            const auto wait_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - wait_started)
                    .count());
            statistics_.frame_slot_wait_ns += wait_ns;
            ++statistics_.frame_slot_wait_samples;
            statistics_.frame_slot_wait_max_ns = std::max(
                statistics_.frame_slot_wait_max_ns,
                wait_ns);
            recordPhase(EFrameTracePhase::FRAME_SLOT_WAIT, wait_ns);
            if (async_ && async_->get().latencyHistogramsEnabled())
            {
                ++statistics_.frame_slot_wait_histogram[
                    lux::exec::detail::asyncLatencyBucket(wait_ns)];
            }
        }
    }

    void FrameCoordinator::recordPhase(
        EFrameTracePhase phase,
        std::uint64_t nanoseconds) noexcept
    {
        const auto index = static_cast<std::size_t>(phase);
        if (!trace_recording_ || index >= kFrameTracePhaseCount)
            return;
        active_trace_.phase_nanoseconds[index] += nanoseconds;
    }

    void FrameCoordinator::commitActiveTrace() noexcept
    {
        if (!trace_recording_)
            return;
        active_trace_.wall_nanoseconds =
            elapsedNanoseconds(active_trace_started_);
        const auto destination =
            (impl_->trace_begin + impl_->trace_size) % kTraceCapacity;
        impl_->traces[destination] = active_trace_;
        if (impl_->trace_size < kTraceCapacity)
        {
            ++impl_->trace_size;
        }
        else
        {
            impl_->trace_begin =
                (impl_->trace_begin + 1u) % kTraceCapacity;
        }
        trace_recording_ = false;
    }

    std::optional<FrameTrace> FrameCoordinator::latestTrace() const noexcept
    {
        if (impl_->trace_size == 0u)
            return std::nullopt;
        const auto index =
            (impl_->trace_begin + impl_->trace_size - 1u) % kTraceCapacity;
        return impl_->traces[index];
    }

    std::vector<FrameTrace> FrameCoordinator::traceHistory() const
    {
        std::vector<FrameTrace> result;
        result.reserve(impl_->trace_size);
        for (std::size_t offset = 0u; offset < impl_->trace_size; ++offset)
        {
            result.push_back(impl_->traces[
                (impl_->trace_begin + offset) % kTraceCapacity]);
        }
        return result;
    }

} // namespace lux::runtime
