#pragma once
#include <lux/engine/editor/sessions/scene/SceneSession.hpp>
namespace lux::editor::ui::detail
{
    inline const char *propertyFailureText(const sessions::SceneFailure &failure) noexcept
    {
        using Error = sessions::ESceneError;
        switch (failure.code)
        {
        case Error::INVALID_ARGUMENT: return "Invalid property value; the last valid preview is retained.";
        case Error::ALLOCATION_FAILURE: return "Not enough memory to prepare this edit. Retry or cancel.";
        case Error::BUSY: return "Another edit is active in this scene.";
        case Error::STALE_CONTENT: return "This edit or its target is no longer current.";
        case Error::STALE_ENTITY: return "The scene preview target is missing; author values are retained.";
        case Error::READ_ONLY: return "This scene is read-only.";
        case Error::HISTORY_FAILURE:
            if (failure.history && failure.history->code == editing::EEditError::STAGING_LIMIT)
                return "This edit exceeds the preparation memory limit.";
            if (failure.history && failure.history->code == editing::EEditError::HISTORY_LIMIT)
                return "This edit exceeds the history memory limit.";
            return "History preparation failed. The edit is retained for retry or cancellation.";
        default: return "The scene cannot accept this edit in its current state.";
        }
    }
}
