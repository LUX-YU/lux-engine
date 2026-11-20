#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/core/Ids.hpp>
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
