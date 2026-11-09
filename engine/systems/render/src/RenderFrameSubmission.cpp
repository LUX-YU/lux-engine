#include <lux/engine/systems/render/RenderFrameSubmission.hpp>

#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/frame/FrameStorage.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <exec/create.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace lux::engine::render
{
    namespace
    {
        RenderFrameSubmission* process_frames = nullptr;
    }

    struct RenderFrameSubmission::Impl final
    {
        struct FrameCarrier final
        {
            lux::render::FrameStorage storage;
            lux::render::RenderFrame packet;
            std::uint64_t resynchronization_epoch = 0;
            bool submitted = false;
        };

        Impl(lux::render::RenderClient& client, std::uint32_t max_frames_ahead)
            : client(client), frames(static_cast<std::size_t>(max_frames_ahead) + 1)
        {
        }

        [[nodiscard]] FrameCarrier* availableFrame() noexcept
        {
            const auto available = std::ranges::find(frames, false, &FrameCarrier::submitted);
            return available == frames.end() ? nullptr : &*available;
        }

        [[nodiscard]] Result prepareFrame() noexcept
        {
            if (failure.type != 0)
            {
                return cxx::unexpected(std::exchange(failure, {}));
            }
            if (next_frame == 0)
            {
                return cxx::unexpected(lux::render::RenderError{lux::render::kRuntimeCapacity});
            }
            building = availableFrame(); // Called only after availability or a failure has been established.
            building->resynchronization_epoch = requires_resynchronization ? resynchronization : 0;
            frame = std::move(building->storage);
            return {};
        }

        [[nodiscard]] bool hasSubmittedFramesThrough(std::uint64_t sequence) const noexcept
        {
            return std::ranges::any_of(frames, [sequence](const auto& slot) noexcept
            {
                return slot.submitted && slot.packet.sequence <= sequence;
            });
        }

        void completeFrame(FrameCarrier& slot, const lux::render::FrameCompletion& completion) noexcept
        {
            if (completion.error.type != 0 || completion.requires_resynchronization)
            {
                ++resynchronization;
                requires_resynchronization = true;
            }
            else if (slot.resynchronization_epoch != 0 && slot.resynchronization_epoch == resynchronization)
            {
                requires_resynchronization = false;
            }
            // Retry only a frame with no accepted rendering submissions, or a queued stale frame
            // explicitly rejected by Server's resynchronization gate. Collect fresh data next time;
            // CPU packet return does not prove GPU completion after a partially accepted submission.
            const bool is_unsubmitted_busy = completion.outcome == lux::render::EFrameOutcome::NOT_SUBMITTED &&
                completion.submitted_batches == 0 && completion.error.type == lux::render::kDeviceBusy;
            const bool is_stale_frame = completion.outcome == lux::render::EFrameOutcome::REJECTED &&
                completion.error.type == lux::render::kFrameResynchronization && completion.requires_resynchronization;
            const bool is_retryable_frame = is_unsubmitted_busy || is_stale_frame;
            if (slot.submitted && completion.error.type != 0 && !is_retryable_frame && failure.type == 0)
            {
                failure = completion.error;
            }
            slot.storage.deliverCompletion(completion);
            slot.storage.reset();
            slot.packet = {};
            slot.submitted = false;

            if (consumption_context && !hasSubmittedFramesThrough(consumption_sequence))
            {
                auto* context = std::exchange(consumption_context, nullptr);
                auto complete = std::exchange(complete_consumption, nullptr);
                complete(context);
            }

            // Only the driver's one suspended begin is borrowed. stdexec owns its OperationState.
            // Clear the borrow before completion: the continuation may immediately submit another frame.
            if (waiting_context)
            {
                auto* context = std::exchange(waiting_context, nullptr);
                auto complete = std::exchange(complete_wait, nullptr);
                complete(context, prepareFrame());
            }
        }

        lux::render::RenderClient& client;
        lux::render::FrameStorage frame;
        std::vector<FrameCarrier> frames;
        FrameCarrier* building = nullptr;
        void* waiting_context = nullptr;
        void (*complete_wait)(void*, Result) noexcept = nullptr;
        void* consumption_context = nullptr;
        void (*complete_consumption)(void*) noexcept = nullptr;
        std::uint64_t consumption_sequence = 0;
        std::uint64_t next_frame = 1;
        std::uint64_t resynchronization = 0;
        bool requires_resynchronization = false;
        lux::render::RenderError failure;
    };

    RenderFrameSubmission::RenderFrameSubmission(lux::render::RenderClient& client, std::uint32_t max_frames_ahead)
    {
        const bool invalid_initialization = process_frames != nullptr || max_frames_ahead == UINT32_MAX ||
                                           !execution::ExecutionRuntime::instance().isMainThread();
        if (invalid_initialization)
        {
            std::terminate(); // EngineContext establishes these process-wide invariants once.
        }
        impl_ = std::make_unique<Impl>(client, max_frames_ahead);
        process_frames = this;
    }

    RenderFrameSubmission::~RenderFrameSubmission() noexcept
    {
        const bool has_pending_wait = impl_->waiting_context || impl_->consumption_context;
        if (has_pending_wait)
        {
            std::terminate(); // The outer driver must finish before its borrowed frame stream.
        }
        discardFrame();
        auto drained = execution::ExecutionRuntime::instance().waitUntil([this]() noexcept
        {
            return std::ranges::none_of(impl_->frames, &Impl::FrameCarrier::submitted);
        });
        if (!drained)
        {
            std::terminate(); // A callback cannot destroy the stream whose completion it must dispatch.
        }
        process_frames = nullptr;
    }

    RenderFrameSubmission& RenderFrameSubmission::instance() noexcept
    {
        if (!process_frames)
        {
            std::terminate(); // Access requires successful engine initialization.
        }
        return *process_frames;
    }

    exec::task<RenderFrameSubmission::Result> RenderFrameSubmission::beginFrame() noexcept
    {
        co_return co_await exec::create<stdexec::set_value_t(Result)>([state = impl_.get()](auto& context) noexcept
        {
            using Context = std::remove_reference_t<decltype(context)>;
            const bool invalid_begin = state->building != nullptr || state->waiting_context != nullptr ||
                                       !execution::ExecutionRuntime::instance().isMainThread();
            if (invalid_begin)
            {
                stdexec::set_value(
                    std::move(context.receiver),
                    Result{cxx::unexpected(lux::render::RenderError{lux::render::kRuntimeState})}
                );
                return;
            }
            if (stdexec::get_stop_token(stdexec::get_env(context.receiver)).stop_requested())
            {
                stdexec::set_value(
                    std::move(context.receiver),
                    Result{cxx::unexpected(lux::render::RenderError{lux::render::kCommandCancelled})}
                );
                return;
            }
            const bool ready = state->failure.type != 0 || state->next_frame == 0 || state->availableFrame() != nullptr;
            if (ready)
            {
                stdexec::set_value(std::move(context.receiver), state->prepareFrame());
                return;
            }
            state->waiting_context = &context;
            state->complete_wait = [](void* value, Result result) noexcept
            {
                auto& context = *static_cast<Context*>(value);
                stdexec::set_value(std::move(context.receiver), std::move(result));
            };
        });
    }

    exec::task<RenderFrameSubmission::Result> RenderFrameSubmission::whenSubmittedFramesConsumed() noexcept
    {
        auto wait = exec::create<stdexec::set_value_t(Result)>([state = impl_.get()](auto& context) noexcept
        {
            using Context = std::remove_reference_t<decltype(context)>;
            if (state->consumption_context)
            {
                stdexec::set_value(
                    std::move(context.receiver), Result{cxx::unexpected(lux::render::RenderError{
                        lux::render::kRuntimeState
                    })}
                );
                return;
            }
            state->consumption_sequence = state->next_frame - 1;
            if (!state->hasSubmittedFramesThrough(state->consumption_sequence))
            {
                stdexec::set_value(std::move(context.receiver), Result{});
                return;
            }
            state->consumption_context = &context;
            state->complete_consumption = [](void* value) noexcept
            {
                auto& pending = *static_cast<Context*>(value);
                stdexec::set_value(std::move(pending.receiver), Result{});
            };
        });
        // Leave completeFrame's producer stack before an owner can release any dependent objects.
        co_return co_await stdexec::continues_on(
            std::move(wait), execution::ExecutionRuntime::instance().mainScheduler()
        );
    }

    lux::render::FrameWriter RenderFrameSubmission::writer() noexcept
    {
        return lux::render::FrameWriter{impl_->frame};
    }

    const lux::render::FrameStorage& RenderFrameSubmission::frame() const noexcept
    {
        return impl_->frame;
    }

    RenderFrameSubmission::Result RenderFrameSubmission::submitFrame() noexcept
    {
        auto* slot = std::exchange(impl_->building, nullptr);
        if (!slot)
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kRuntimeState});
        }
        slot->storage = std::move(impl_->frame);
        slot->packet = slot->storage.seal(impl_->next_frame++, slot->resynchronization_epoch != 0);
        slot->submitted = true;
        auto completed = stdexec::then(
            impl_->client.request<lux::render::SubmitFrame>(slot->packet),
            [state = impl_.get(), slot](lux::render::FrameCompletion completion) noexcept
            {
                state->completeFrame(*slot, completion);
            }
        );
        auto accepted = execution::ExecutionRuntime::instance().spawn(std::move(completed));
        if (!accepted)
        {
            const auto failure = execution::toError(accepted.error());
            slot->submitted = false; // No receiver was started; return this failure directly.
            impl_->completeFrame(*slot, {
                failure, slot->packet.sequence, lux::render::EFrameOutcome::NOT_SUBMITTED, 0, true
            });
            return cxx::unexpected(failure);
        }
        return {};
    }

    void RenderFrameSubmission::discardFrame() noexcept
    {
        auto* slot = std::exchange(impl_->building, nullptr);
        if (!slot)
        {
            return;
        }
        slot->storage = std::move(impl_->frame);
        impl_->completeFrame(*slot, {
            {lux::render::kCommandCancelled}, 0, lux::render::EFrameOutcome::NOT_SUBMITTED, 0, true
        });
    }
} // namespace lux::engine::render
