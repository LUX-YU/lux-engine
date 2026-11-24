#pragma once

#include <lux/engine/render/resource/ResourceId.hpp>
#include <utility>

namespace lux::render { struct FinalBufferVersion; struct ResourceRegistration; }

namespace lux::render::detail
{
    struct StorageBindingOps;

    [[nodiscard]] constexpr auto resourceOrder(ResourceInstanceId id) noexcept
    {
        return std::pair{id.index, id.gen};
    }

    // Installed Jobs keep the resource alive until the selected round and its copy receipts settle.
    struct InstalledFrameResource final
    {
        ResourceInstanceId id;
        void* instance;
        const StorageBindingOps* ops;
    };
}

namespace lux::render::runtime_detail
{
    // Resolved once during installation. The factory and instance are held by the candidate resource list.
    struct ResolvedResource final
    {
        ResourceInstanceId id;
        void* instance = nullptr;
        const ResourceRegistration* factory = nullptr;
        // Explicit JobBinding inputs remain immutable for this synchronous installation.
        const FinalBufferVersion* publication = nullptr;

        [[nodiscard]] detail::InstalledFrameResource frameSource() const noexcept;
    };
}
