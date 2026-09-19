#include <lux/engine/scene/RenderSyncPipeline.hpp>
#include <lux/engine/scene/detail/RenderSyncStorage.hpp>

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
        explicit Impl(StageList value, detail::RenderSyncStorage &channel,
                      std::shared_ptr<const RenderSystemMetadata> owner)
            : metadata(std::move(owner)), stages(std::move(value)), storage(&channel)
        {
            for (auto &stage : stages)
            {
                stage->requestFullSync();
            }
        }

        std::shared_ptr<const RenderSystemMetadata> metadata;
        StageList stages;
        detail::RenderSyncStorage *storage;
        bool full_sync_requested{true};
    };

    RenderSyncPipeline::RenderSyncPipeline(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    RenderSyncPipeline::~RenderSyncPipeline() noexcept
    {
        impl_->stages.clear();
        impl_->storage->producer_closed = true;
    }

    lux::cxx::expected<std::unique_ptr<RenderSyncPipeline>, RenderSyncPipelineFailure> RenderSyncPipeline::create(
        StageList stages, detail::RenderSyncStorage &storage, std::shared_ptr<const RenderSystemMetadata> metadata)
    {
        if (std::ranges::any_of(stages, [](const auto &stage) { return !stage; }))
        {
            return lux::cxx::unexpected(RenderSyncPipelineFailure{ERenderSyncPipelineError::INVALID_STAGE_LIST});
        }
        return std::unique_ptr<RenderSyncPipeline>(
            new RenderSyncPipeline(std::make_unique<Impl>(std::move(stages), storage, std::move(metadata))));
    }

    ERenderPublishResult RenderSyncPipeline::tryPublish() noexcept
    {
        auto &state = *impl_;
        if (state.storage->stopped)
        {
            return ERenderPublishResult::FAILED;
        }
        const bool has_changes =
            state.full_sync_requested ||
            std::ranges::any_of(state.stages, [](const auto &stage) { return stage->hasPendingChanges(); });
        if (!has_changes)
        {
            return ERenderPublishResult::NO_CHANGES;
        }
        if (state.storage->prepared)
        {
            ++state.storage->statistics.backpressured;
            return ERenderPublishResult::BACKPRESSURED;
        }

        auto *program = &state.storage->update;
        render::RenderProgramBuilder<> builder{*program};
        builder.begin();
        program->kind = render::ERenderProgramKind::StateUpdate;

        bool has_commands = false;
        for (auto &stage : state.stages)
        {
            const auto result = stage->prepare(builder);
            if (result == ERenderSyncPrepareResult::FAILED)
            {
                for (auto &discard_stage : state.stages)
                {
                    discard_stage->discardPrepared();
                }
                return ERenderPublishResult::FAILED;
            }
            has_commands = has_commands || result == ERenderSyncPrepareResult::PREPARED_COMMANDS;
        }

        if (!builder.valid())
        {
            for (auto &stage : state.stages)
            {
                stage->discardPrepared();
            }
            return ERenderPublishResult::FAILED;
        }
        if (!has_commands)
        {
            for (auto &stage : state.stages)
            {
                stage->commitPrepared();
            }
            state.full_sync_requested = false;
            return ERenderPublishResult::NO_CHANGES;
        }
        for (auto &stage : state.stages)
        {
            stage->commitPrepared();
        }
        state.storage->prepared = true;
        ++state.storage->statistics.published;
        state.storage->statistics.pending = 1;
        state.storage->statistics.high_water = 1;
        const bool was_full_sync = std::exchange(state.full_sync_requested, false);
        return was_full_sync ? ERenderPublishResult::FULL_SYNC_PUBLISHED : ERenderPublishResult::PUBLISHED;
    }

    void RenderSyncPipeline::requestFullSync() noexcept
    {
        auto &state = *impl_;
        state.full_sync_requested = true;
        for (auto &stage : state.stages)
        {
            stage->requestFullSync();
        }
    }

} // namespace lux::scene
