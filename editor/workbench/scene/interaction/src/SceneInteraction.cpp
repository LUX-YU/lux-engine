#include <lux/engine/editor/scene/SceneInteraction.hpp>
#include <algorithm>
#include <exception>

namespace lux::editor::scene
{
    namespace
    {
        SceneEditError selectionFailure(const RunFailure& failure) noexcept
        {
            if (const auto* error = std::get_if<ERunError>(&failure.cause))
            {
                if (*error == ERunError::BUSY)
                    return SceneEditError{ESceneEditError::BUSY};
                if (*error == ERunError::WRONG_THREAD)
                    return SceneEditError{ESceneEditError::WRONG_THREAD};
                if (*error == ERunError::INVALID_ID)
                    return SceneEditError{ESceneEditError::STALE_OBJECT};
            }
            return SceneEditError{ESceneEditError::INVALID_SOURCE};
        }
    }
    SceneInteractionGroup::SceneInteractionGroup(
        sessions::TSessionAccess<SceneSession> access,
        sessions::TSessionKey<SceneSession> key,
        InteractionGroupId id,
        std::optional<RunInspectAccess> runs
    ) noexcept
        : access_(access), key_(key), id_(id), runs_(runs)
    {}
    SceneInteractionGroup::SceneInteractionGroup(RunInspectAccess runs, RunId run, InteractionGroupId id) noexcept
        : id_(id), runs_(runs), run_(run)
    {}
    SceneInteractionGroup::~SceneInteractionGroup() noexcept
    {
        if (gesture_ && !cancel())
            std::terminate(); // Destruction inside an admitted callback violates the owner lifetime contract.
    }
    SceneEditResult<void> SceneInteractionGroup::begin(std::string label)
    {
        if (!access_)
            return lux::cxx::unexpected(sessions::ESessionError::INVALID_ARGUMENT);
        if (gesture_)
            return lux::cxx::unexpected(sessions::ESessionError::BUSY);
        auto info = access_->describe(*key_);
        if (!info)
            return lux::cxx::unexpected(info.error());
        if (info->admission != sessions::EEditAdmission::AVAILABLE)
            return lux::cxx::unexpected(sessions::ESessionError::BUSY);
        gesture_.emplace(SceneEditBatch{info->current, std::move(label), {}});
        return {};
    }
    SceneEditResult<void> SceneInteractionGroup::preview(std::vector<VSceneEdit>& candidate)
    {
        if (!gesture_)
            return lux::cxx::unexpected(sessions::ESessionError::NOT_PREPARED);
        auto info = access_->describe(*key_);
        if (!info)
            return lux::cxx::unexpected(info.error());
        if (info->current != gesture_->expected)
            return lux::cxx::unexpected(sessions::ESessionError::STALE_CONTENT);
        auto owner = access_->read(*key_);
        if (!owner)
            return lux::cxx::unexpected(owner.error());
        auto read = owner->get().read();
        if (!read)
            return lux::cxx::unexpected(read.error());
        return read->withRead([&](const SceneReadView&) -> SceneEditResult<void> {
            gesture_->edits.swap(candidate);
            candidate.clear(); // Old preview payload and its code are released under the original gate.
            return {};
        });
    }
    SceneEditResult<SceneEditReceipt> SceneInteractionGroup::commit()
    {
        if (!gesture_)
            return lux::cxx::unexpected(sessions::ESessionError::NOT_PREPARED);
        auto info = access_->describe(*key_);
        if (!info)
            return lux::cxx::unexpected(info.error());
        if (info->admission != sessions::EEditAdmission::AVAILABLE)
            return lux::cxx::unexpected(sessions::ESessionError::BUSY);
        if (info->current != gesture_->expected)
            return lux::cxx::unexpected(sessions::ESessionError::STALE_CONTENT);
        auto owner = access_->edit(*key_);
        if (!owner)
            return lux::cxx::unexpected(owner.error());
        auto batch = std::move(*gesture_);
        gesture_.reset();
        return owner->get().apply(std::move(batch));
    }
    SceneEditResult<void> SceneInteractionGroup::cancel()
    {
        if (!gesture_)
            return {};
        auto owner = access_->read(*key_);
        if (!owner)
        {
            if (owner.error() != sessions::ESessionError::STALE_SESSION)
                return lux::cxx::unexpected(owner.error());
            // Only a confirmed stale identity can release input without borrowing a live Session gate.
            auto discarded = std::move(*gesture_);
            gesture_.reset();
            return {};
        }
        auto read = owner->get().read();
        if (!read)
            return lux::cxx::unexpected(read.error());
        return read->withRead([&](const SceneReadView&) -> SceneEditResult<void> {
            auto discarded = std::move(*gesture_);
            gesture_.reset(); // Input destruction follows below, while this read admission is still held.
            return {};
        });
    }
    SceneEditResult<void> SceneInteractionGroup::select(SceneSelection selection)
    {
        if (run_)
        {
            auto described = runs_->describe(*run_);
            if (!described)
            {
                return lux::cxx::unexpected(selectionFailure(described.error()));
            }
            for (const auto& target : selection.objects)
            {
                const auto* ref = std::get_if<RunningObjectRef>(&target);
                if (!ref || ref->run != *run_ || !runs_->contains(*ref))
                    return lux::cxx::unexpected(SceneEditError{ESceneEditError::STALE_OBJECT});
            }
            selection_ = std::move(selection);
            return {};
        }
        auto owner = access_->read(*key_);
        if (!owner)
            return lux::cxx::unexpected(owner.error());
        auto read = owner->get().read();
        if (!read)
            return lux::cxx::unexpected(read.error());
        for (const auto& target : selection.objects)
        {
            const bool valid = std::visit(
                [&](const auto& ref) {
                    if constexpr (std::same_as<std::decay_t<decltype(ref)>, SceneObjectRef>)
                        return read->contains(ref);
                    else
                        return runs_ && runs_->contains(ref);
                },
                target
            );
            if (!valid)
                return lux::cxx::unexpected(SceneEditError{ESceneEditError::STALE_OBJECT});
        }
        selection_ = std::move(selection);
        return {};
    }
    SceneEditResult<void> SceneInteractionGroup::synchronize()
    {
        if (run_)
        {
            auto described = runs_->describe(*run_);
            if (!described)
            {
                const auto* error = std::get_if<ERunError>(&described.error().cause);
                if (!error || *error != ERunError::INVALID_ID)
                    return lux::cxx::unexpected(selectionFailure(described.error()));
                selection_.objects.clear();
                return {};
            }
            std::erase_if(selection_.objects, [&](const auto& target) {
                const auto* ref = std::get_if<RunningObjectRef>(&target);
                return !ref || !runs_->contains(*ref);
            });
            return {};
        }
        auto info = access_->describe(*key_);
        if (!info && info.error() != sessions::ESessionError::STALE_SESSION)
            return lux::cxx::unexpected(info.error());
        if (!info || (gesture_ && info->current != gesture_->expected))
        {
            auto cancelled = cancel();
            if (!cancelled)
                return cancelled;
        }
        if (!info)
        {
            selection_.objects.clear();
            return {};
        }
        auto owner = access_->read(*key_);
        if (!owner)
            return lux::cxx::unexpected(owner.error());
        auto read = owner->get().read();
        if (!read)
            return lux::cxx::unexpected(read.error());
        std::erase_if(selection_.objects, [&](const auto& target) {
            return !std::visit(
                [&](const auto& ref) {
                    if constexpr (std::same_as<std::decay_t<decltype(ref)>, SceneObjectRef>)
                        return read->contains(ref);
                    else
                        return runs_ && runs_->contains(ref);
                },
                target
            );
        });
        return {};
    }
}
