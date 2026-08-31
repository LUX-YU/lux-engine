#include <lux/engine/scene/RenderSystem.hpp>

#include <lux/engine/function/render/client/BoundedSpscFrameRing.hpp>
#include <lux/engine/function/render/client/RenderProgram.hpp>

#include <algorithm>
#include <new>
#include <utility>

namespace lux::scene
{
    struct RenderSystem::Impl final
    {
        explicit Impl(StageList value) : stages(std::move(value)), updates(1U)
        {
        }

        StageList stages;
        lux::cxx::BoundedSpscFrameRing<render::RenderProgram<>, 3> updates;
        bool full_sync_requested{true};
        bool forward_pending{false};
    };

    RenderSystem::RenderSystem(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }

    RenderSystem::~RenderSystem() noexcept = default;

    lux::cxx::expected<std::unique_ptr<RenderSystem>, RenderSystemFailure>
    RenderSystem::create(StageList stages) noexcept
    {
        const bool has_null_stage = std::ranges::any_of(stages, [](const auto& stage) { return stage == nullptr; });
        if (stages.empty() || has_null_stage)
        {
            return lux::cxx::unexpected(RenderSystemFailure{ERenderSystemError::INVALID_STAGE_LIST});
        }
        try
        {
            return std::unique_ptr<RenderSystem>{new RenderSystem{std::make_unique<Impl>(std::move(stages))}};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(RenderSystemFailure{ERenderSystemError::ALLOCATION_FAILURE});
        }
    }

    ERenderPublishResult RenderSystem::tryPublish() noexcept
    {
        auto& state = *impl_;
        const bool has_changes = state.full_sync_requested || std::ranges::any_of(
            state.stages,
            [](const auto& stage) { return stage->hasPendingChanges(); }
        );
        if (!has_changes)
        {
            return ERenderPublishResult::NO_CHANGES;
        }
        if (state.updates.pendingFrames() >= state.updates.maxPendingFrames())
        {
            return ERenderPublishResult::BACKPRESSURED;
        }

        auto* program = state.updates.tryBeginWrite();
        if (program == nullptr)
        {
            return ERenderPublishResult::BACKPRESSURED;
        }
        render::RenderProgramBuilder<> builder{*program};
        builder.begin();
        program->kind = render::ERenderProgramKind::StateUpdate;

        bool has_commands = false;
        std::size_t prepared_count = 0U;
        for (auto& stage : state.stages)
        {
            const auto result = stage->prepare(builder);
            if (result == ERenderSyncPrepareResult::FAILED)
            {
                for (std::size_t index = 0U; index <= prepared_count && index < state.stages.size(); ++index)
                {
                    state.stages[index]->discardPrepared();
                }
                return ERenderPublishResult::FAILED;
            }
            has_commands = has_commands || result == ERenderSyncPrepareResult::PREPARED_COMMANDS;
            ++prepared_count;
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
        if (!state.updates.publishWrite())
        {
            for (auto& stage : state.stages)
            {
                stage->discardPrepared();
            }
            return ERenderPublishResult::BACKPRESSURED;
        }
        for (auto& stage : state.stages)
        {
            stage->commitPrepared();
        }
        const bool was_full_sync = std::exchange(state.full_sync_requested, false);
        return was_full_sync ? ERenderPublishResult::FULL_SYNC_PUBLISHED : ERenderPublishResult::PUBLISHED;
    }

    void RenderSystem::requestFullSync() noexcept
    {
        auto& state = *impl_;
        state.full_sync_requested = true;
        for (auto& stage : state.stages)
        {
            stage->requestFullSync();
        }
    }

    ERenderForwardResult RenderSystem::tryForwardUpdate(render::RenderProgramSession& session) noexcept
    {
        auto& state = *impl_;
        if (session.isStopping())
        {
            return ERenderForwardResult::STOPPING;
        }
        if (session.hasPendingSubmit() && !session.retryPendingSubmit())
        {
            return ERenderForwardResult::BACKPRESSURED;
        }
        if (!state.forward_pending)
        {
            if (!state.updates.tryAcquireRead())
            {
                return ERenderForwardResult::NO_UPDATE;
            }
            state.forward_pending = true;
        }
        if (!session.trySubmitPrepared(state.updates.currentRead()))
        {
            return ERenderForwardResult::BACKPRESSURED;
        }
        state.forward_pending = false;
        return ERenderForwardResult::FORWARDED;
    }
} // namespace lux::scene
