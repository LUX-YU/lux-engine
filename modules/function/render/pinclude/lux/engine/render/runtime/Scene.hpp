#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/inputs/CameraInputs.hpp>
#include <vector>

namespace lux::render::runtime_detail
{
    struct RenderView final
    {
        std::vector<RenderJobId> jobs;
        CameraInput camera;
        std::uint64_t camera_revision = 1;
    };

    struct RenderScene final
    {
        cxx::StableSlotMap<RenderView, ViewTag> views;
    };
} // namespace lux::render::runtime_detail
