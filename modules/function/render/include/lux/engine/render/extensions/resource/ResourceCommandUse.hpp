#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    class ResourceRegistry;

    // An admitted command borrows the existing Resource owner through terminal delivery.
    // This record never owns Storage and never allocates another lifetime service.
    class LUX_RENDER_PUBLIC ResourceCommandUse final
    {
    public:
        ResourceCommandUse() noexcept = default;
        ~ResourceCommandUse() noexcept;
        ResourceCommandUse(ResourceCommandUse&&) noexcept;
        ResourceCommandUse& operator=(ResourceCommandUse&&) noexcept;
        ResourceCommandUse(const ResourceCommandUse&) = delete;
        ResourceCommandUse& operator=(const ResourceCommandUse&) = delete;

        [[nodiscard]] void* get() const noexcept
        {
            return instance_;
        }

        void reset() noexcept;

    private:
        friend class ResourceRegistry;
        ResourceCommandUse(ResourceRegistry&, ResourceInstanceId, void*) noexcept;
        ResourceRegistry* registry_ = nullptr;
        ResourceInstanceId id_;
        void* instance_ = nullptr;
    };
} // namespace lux::render
