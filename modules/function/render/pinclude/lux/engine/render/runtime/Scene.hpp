#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/inputs/CameraInputs.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>
#include <vector>

namespace lux::render::detail
{
    class DeviceEntry;
}

namespace lux::render::runtime_detail
{
    struct RenderScene;

    struct RenderView final
    {
        explicit RenderView(RenderScene& parent) noexcept;

        RenderScene& scene;
        ResourceSet storages;
        std::vector<RenderJobId> jobs;
        CameraInput camera;
        std::uint64_t camera_revision = 1;
    };

    struct RenderScene final
    {
        explicit RenderScene(detail::DeviceEntry& entry) noexcept;

        detail::DeviceEntry& device;
        ResourceSet storages;
        cxx::StableSlotMap<RenderView, ViewTag> views;
    };
} // namespace lux::render::runtime_detail
