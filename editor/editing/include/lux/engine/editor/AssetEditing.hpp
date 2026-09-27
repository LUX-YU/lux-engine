#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <optional>

namespace lux::editor
{
    enum class EAssetChange : std::uint8_t
    {
        OPEN,
        NEW,
        CLEAR,
        EXIT
    };
    enum class EAssetChangeDecision : std::uint8_t
    {
        SAVE,
        DISCARD,
        CANCEL
    };
    enum class EAssetEditPhase : std::uint8_t
    {
        IDLE,
        REVIEW,
        SAVING,
        READING,
        CONFIGURING,
        PREPARING,
        EXIT_READY
    };

    // Facts owned by a concrete tool. The root only activates tools and coordinates exit readiness.
    struct AssetEditStatus final
    {
        EAssetEditPhase phase{EAssetEditPhase::IDLE};
        EAssetChange change{EAssetChange::CLEAR};
        asset::AssetId target;
        std::optional<EditorFailure> failure;
    };
}
