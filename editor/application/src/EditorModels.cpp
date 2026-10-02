#include <lux/engine/editor/application/EditorApplicationImpl.hpp>

namespace lux::editor::application
{
    namespace
    {
        bool retryable(const scene::ModelCreationFailure& failure) noexcept
        {
            return std::visit(
                [](const auto& error) {
                    using Error = std::decay_t<decltype(error)>;
                    if constexpr (std::same_as<Error, scene::SceneEditError>)
                        return error.code == scene::ESceneEditError::BUSY ||
                               (error.code == scene::ESceneEditError::SESSION &&
                                error.session == sessions::ESessionError::BUSY);
                    else if constexpr (std::same_as<Error, project::VProjectQueryFailure>)
                    {
                        const auto* query = std::get_if<project::EProjectQueryError>(&error);
                        return query && *query == project::EProjectQueryError::BUSY;
                    }
                    else
                        return false;
                },
                failure.cause
            );
        }
    }
    void EditorApplication::Impl::receiveModel(scene::ModelPlacement placement)
    {
        if (phase_ != EApplicationPhase::RUNNING)
        {
            result_failure_ = EditorFailure{EEditorError::CLOSING, "model.admission"};
            return;
        }
        if (model_placements_.size() == 32 || next_model_ == UINT64_MAX)
        {
            result_failure_ = EditorFailure{
                EEditorError::CAPACITY,
                "model.results",
                0,
                "Acknowledge previous model results before inserting another model."
            };
            return;
        }
        // Only capture UI intent here: this may run in a DIRECT callback during drawing.
        model_placements_.push_back({next_model_++, std::move(placement)});
    }
    void EditorApplication::Impl::settleModels()
    {
        for (auto& entry : model_placements_)
        {
            if (entry.result || entry.failure)
                continue;
            if (phase_ == EApplicationPhase::DRAINING)
                entry.cancel_requested = true;
            if (!entry.operation)
            {
                if (entry.cancel_requested)
                {
                    entry.result.emplace(cxx::unexpected(scene::ModelCreationFailure{process::TaskCancelled{}}));
                    continue;
                }
                if (phase_ != EApplicationPhase::RUNNING)
                    continue;
                auto reads = project_->captureAssetReads();
                if (!reads)
                {
                    if (reads.error().code != EEditorError::BUSY)
                        entry.failure = reads.error();
                    continue;
                }
                auto started = scene::ModelCreationOperation::start(
                    engine_->execution(),
                    sessions_.access<scene::SceneSession>(),
                    project_->catalogModel(),
                    std::move(*reads),
                    registrations_.components,
                    entry.placement
                );
                if (!started)
                {
                    if (!retryable(started.error()))
                        entry.result.emplace(cxx::unexpected(std::move(started.error())));
                    continue;
                }
                entry.operation = std::move(*started);
            }
            if (entry.cancel_requested)
                entry.operation->cancel();
            if (!entry.operation->settled())
                continue;
            auto result = entry.operation->commit();
            if (!result && retryable(result.error()))
                continue;
            entry.result.emplace(std::move(result));
            // The transport completion has actually returned. Deleting the owner cannot wait for IO here.
            entry.operation.reset();
        }
    }
}
