#pragma once

#include <lux/engine/render/resource/ResourceId.hpp>
#include <lux/engine/render/resource/ResourceRequirement.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/visibility.h>
#include <vector>

namespace lux::render::detail
{
    class DeviceSession;
    class DeviceEntry;
    class TransformStorage;
    class SceneViewRow;
    class SceneViewBinding;
    class SceneViewStorage;
    class FrameProgress;
    class MaterialStorage;
    class ResourceUploader;
} // namespace lux::render::detail

namespace lux::render
{
    class ResourceRegistry;
    class RenderServer;

    // Synchronous factory input. Instances retain only the dependencies they actually use.
    struct ResourceCreateContext final
    {
        detail::DeviceSession& device;
        detail::ResourceUploader* uploads;
        std::uint32_t frame_count;
        ResourceRegistry& resources;

        // Acquire the default dependency in its owning scope and retain it for this instance.
        [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<ResourceInstanceId> require(ResourceNameView dependency) noexcept;

    private:
        friend class ResourceRegistry;
        friend class ResourceSet;
        friend class RasterProgramSet;
        ResourceInstanceId constructing_;
        std::vector<ResourceInstanceId>* created_ = nullptr;
        friend class detail::DeviceEntry;
        friend class detail::TransformStorage;
        friend class detail::MaterialStorage;
        friend class detail::SceneViewBinding;
        friend class RenderServer;
        detail::SceneViewRow* view_row_ = nullptr;
        detail::SceneViewStorage* view_table_ = nullptr;
        std::uint32_t view_index_ = 0;
        explicit ResourceCreateContext(detail::DeviceEntry&) noexcept;
        detail::DeviceEntry& device_owner_;
        const detail::FrameProgress& retirement_progress_;
    };
} // namespace lux::render
