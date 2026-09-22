#pragma once

#include <lux/engine/editor/editing/EditTypes.hpp>
#include <lux/engine/scene/RenderAssets.hpp>

namespace lux::editor::scene
{
// A document observation, not a second resource request/retirement owner.
struct SceneResourceSnapshot final
{
    editing::HistoryId history;
    std::uint64_t revision{};
    std::vector<lux::scene::RenderAssetStatus> rows;
};
} // namespace lux::editor::scene
