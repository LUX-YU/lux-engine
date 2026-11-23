#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/runtime/JobId.hpp>
#include <lux/engine/render/runtime/SceneId.hpp>
#include <vector>

namespace lux::render::runtime_detail
{
    struct RenderView final
    {
        std::vector<RenderJobId> jobs;
    };

    struct RenderScene final
    {
        cxx::StableSlotMap<RenderView, ViewTag> views;
    };
} // namespace lux::render::runtime_detail
