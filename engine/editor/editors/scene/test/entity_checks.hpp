#pragma once

#include <algorithm>
#include <vector>
#include <lux/engine/editor/scene/SceneEditor.hpp>

inline std::vector<lux::editor::scene::SceneEntityRef> entitySnapshot(lux::editor::scene::SceneEditor &scene)
{
    std::vector<lux::editor::scene::SceneEntityRef> result;
    for (const auto &row : scene.objects())
    {
        result.push_back(row.object);
    }
    return result;
}

inline std::vector<lux::editor::scene::SceneEntityRef> newEntities(
    lux::editor::scene::SceneEditor &scene, const std::vector<lux::editor::scene::SceneEntityRef> &previous)
{
    std::vector<lux::editor::scene::SceneEntityRef> result;
    for (const auto &row : scene.objects())
    {
        if (std::ranges::find(previous, row.object) == previous.end())
        {
            result.push_back(row.object);
        }
    }
    return result;
}
