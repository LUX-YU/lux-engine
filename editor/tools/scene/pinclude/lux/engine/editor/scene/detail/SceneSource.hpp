#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/scene/ScenePackage.hpp>

namespace lux::editor::scene::detail
{
    [[nodiscard]] EditorResult<lux::scene::ScenePackage> copySceneSource(
        const lux::scene::ScenePackage&,
        asset::AssetId,
        std::stop_token = {}
    ) noexcept;
}
