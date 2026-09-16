#include <lux/engine/scene/RenderSyncPipeline.hpp>
#include <lux/engine/scene/detail/RenderSyncStorage.hpp>

#include <lux/engine/function/render/client/BoundedSpscFrameRing.hpp>
#include <lux/engine/function/render/client/RenderProgram.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <new>
#include <utility>

namespace lux::scene
{
    struct RenderSyncPipeline::Impl final
    {
        explicit Impl(StageList value, std::shared_ptr<detail::RenderSyncStorage> channel,
                      std::shared_ptr<const SceneMetaManager> owner)
            : metadata(std::move(owner)), stages(std::move(value)), storage(std::move(channel))
        {
            for (auto& stage : stages)
            {
                stage->requestFullSync();
            }
        }

        std::shared_ptr<const SceneMetaManager> metadata;
        StageList stages;
        std::shared_ptr<detail::RenderSyncStorage> storage;
        bool full_sync_requested{true};
    };

    RenderSyncPipeline::RenderSyncPipeline(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }

    RenderSyncPipeline::~RenderSyncPipeline() noexcept
    {
        impl_->stages.clear();
        impl_->storage->producer_closed.store(true, std::memory_order_release);
        impl_->storage->notify();
    }

    bool RenderSyncPipeline::waitForCapacity(std::stop_token stop) const noexcept
    {
        auto &storage = *impl_->storage;
        std::stop_callback wake(stop, [&storage] { storage.notify(); });
        for (;;)
        {
            const auto observed = storage.progress.load(std::memory_order_acquire);
            if (stop.stop_requested() || storage.consumer_closed.load(std::memory_order_acquire))
            {
                return false;
            }
            if (storage.updates.pendingFrames() < storage.updates.maxPendingFrames())
            {
                return true;
            }
            storage.progress.wait(observed, std::memory_order_acquire);
        }
    }

    lux::cxx::expected<std::unique_ptr<RenderSyncPipeline>, RenderSyncPipelineFailure> RenderSyncPipeline::create(
        StageList stages, std::shared_ptr<detail::RenderSyncStorage> storage,
        std::shared_ptr<const SceneMetaManager> metadata)
    {
        if (!storage || std::ranges::any_of(stages, [](const auto &stage) { return !stage; }))
        {
            return lux::cxx::unexpected(RenderSyncPipelineFailure{ERenderSyncPipelineError::INVALID_STAGE_LIST});
        }
        return std::unique_ptr<RenderSyncPipeline>(
            new RenderSyncPipeline(std::make_unique<Impl>(std::move(stages), std::move(storage), std::move(metadata))));
    }

    ERenderPublishResult RenderSyncPipeline::tryPublish() noexcept
    {
        auto& state = *impl_;
        if (state.storage->consumer_closed.load(std::memory_order_acquire))
        {
            return ERenderPublishResult::FAILED;
        }
        const bool has_changes = state.full_sync_requested || std::ranges::any_of(
            state.stages,
            [](const auto& stage) { return stage->hasPendingChanges(); }
        );
        if (!has_changes)
        {
            return ERenderPublishResult::NO_CHANGES;
        }
        if (state.storage->updates.pendingFrames() >= state.storage->updates.maxPendingFrames())
        {
            state.storage->backpressured.fetch_add(1, std::memory_order_relaxed);
            return ERenderPublishResult::BACKPRESSURED;
        }

        auto *program = state.storage->updates.tryBeginWrite();
        if (program == nullptr)
        {
            state.storage->backpressured.fetch_add(1, std::memory_order_relaxed);
            return ERenderPublishResult::BACKPRESSURED;
        }
        render::RenderProgramBuilder<> builder{*program};
        builder.begin();
        program->kind = render::ERenderProgramKind::StateUpdate;

        bool has_commands = false;
        for (auto& stage : state.stages)
        {
            const auto result = stage->prepare(builder);
            if (result == ERenderSyncPrepareResult::FAILED)
            {
                for (auto& discard_stage : state.stages)
                {
                    discard_stage->discardPrepared();
                }
                return ERenderPublishResult::FAILED;
            }
            has_commands = has_commands || result == ERenderSyncPrepareResult::PREPARED_COMMANDS;
        }

        if (!builder.valid())
        {
            for (auto& stage : state.stages)
            {
                stage->discardPrepared();
            }
            return ERenderPublishResult::FAILED;
        }
        if (!has_commands)
        {
            for (auto& stage : state.stages)
            {
                stage->commitPrepared();
            }
            state.full_sync_requested = false;
            return ERenderPublishResult::NO_CHANGES;
        }
        if (!state.storage->updates.publishWrite())
        {
            for (auto& stage : state.stages)
            {
                stage->discardPrepared();
            }
            state.storage->backpressured.fetch_add(1, std::memory_order_relaxed);
            return ERenderPublishResult::BACKPRESSURED;
        }
        for (auto& stage : state.stages)
        {
            stage->commitPrepared();
        }
        state.storage->published.fetch_add(1, std::memory_order_relaxed);
        const bool was_full_sync = std::exchange(state.full_sync_requested, false);
        return was_full_sync ? ERenderPublishResult::FULL_SYNC_PUBLISHED : ERenderPublishResult::PUBLISHED;
    }

    void RenderSyncPipeline::requestFullSync() noexcept
    {
        auto& state = *impl_;
        state.full_sync_requested = true;
        for (auto& stage : state.stages)
        {
            stage->requestFullSync();
        }
    }

    RenderSyncConsumer::RenderSyncConsumer(std::shared_ptr<detail::RenderSyncStorage> storage) noexcept
        : storage_(std::move(storage))
    {
    }

    RenderSyncConsumer::~RenderSyncConsumer()
    {
        close();
    }
    RenderSyncConsumer::RenderSyncConsumer(RenderSyncConsumer &&other) noexcept
        : storage_(std::move(other.storage_)), forward_pending_(std::exchange(other.forward_pending_, false))
    {
    }
    RenderSyncConsumer &RenderSyncConsumer::operator=(RenderSyncConsumer &&other) noexcept
    {
        if (this != &other)
        {
            close();
            storage_ = std::move(other.storage_);
            forward_pending_ = std::exchange(other.forward_pending_, false);
        }
        return *this;
    }
    void RenderSyncConsumer::close() noexcept
    {
        if (storage_)
        {
            stop();
            storage_.reset();
        }
    }

    void RenderSyncConsumer::stop() noexcept
    {
        storage_->consumer_closed.store(true, std::memory_order_release);
        storage_->notify();
    }

    void RenderSyncConsumer::retireAfterBackendStopped() noexcept
    {
        assert(producerClosed());
        // Both ring roles are now exclusively Main-owned. Include read, queued,
        // and unused write slots: a failed prepare can retain attachments too.
        storage_->retired_unforwarded =
            storage_->published.load(std::memory_order_relaxed) - storage_->forwarded.load(std::memory_order_relaxed);
        forward_pending_ = false;
        auto &ring = storage_->updates;
        ring.currentRead().clear_keep_capacity();
        while (ring.tryAcquireRead())
        {
            ring.currentRead().clear_keep_capacity();
        }
        for (unsigned i = 0; i < 3; ++i)
        {
            auto *slot = ring.tryBeginWrite();
            assert(slot);
            slot->clear_keep_capacity();
            const bool published = ring.publishWrite();
            const bool acquired = ring.tryAcquireRead();
            assert(published && acquired);
        }
    }

    ERenderForwardResult RenderSyncConsumer::tryForwardUpdate(render::RenderProgramSession &session) noexcept
    {
        if (session.isStopping())
        {
            stop();
            return ERenderForwardResult::STOPPING;
        }
        if (session.hasPendingSubmit() && !session.retryPendingSubmit())
        {
            return ERenderForwardResult::BACKPRESSURED;
        }
        if (!forward_pending_)
        {
            if (!storage_->updates.tryAcquireRead())
            {
                return ERenderForwardResult::NO_UPDATE;
            }
            forward_pending_ = true;
            storage_->notify();
        }
        if (!session.trySubmitPrepared(storage_->updates.currentRead()))
        {
            return ERenderForwardResult::BACKPRESSURED;
        }
        forward_pending_ = false;
        storage_->forwarded.fetch_add(1, std::memory_order_relaxed);
        return ERenderForwardResult::FORWARDED;
    }

    bool RenderSyncConsumer::hasPendingUpdate() const noexcept
    {
        return forward_pending_ || storage_->updates.pendingFrames() != 0;
    }

    bool RenderSyncConsumer::producerClosed() const noexcept
    {
        return storage_->producer_closed.load(std::memory_order_acquire);
    }
} // namespace lux::scene
