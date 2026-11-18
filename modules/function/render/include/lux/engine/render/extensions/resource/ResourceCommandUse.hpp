#pragma once

#include <lux/engine/render/commands/CommandBindingLocation.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
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

        [[nodiscard]] CommandBindingLocation binding() const noexcept { return binding_; }
        [[nodiscard]] cxx::basic_type_info implementationType() const noexcept { return implementation_; }

        void reset() noexcept;

    private:
        friend class ResourceRegistry;
        ResourceCommandUse(
            ResourceRegistry&, ResourceInstanceId, void*, CommandBindingLocation, cxx::basic_type_info
        ) noexcept;
        ResourceRegistry* registry_ = nullptr;
        ResourceInstanceId id_;
        void* instance_ = nullptr;
        CommandBindingLocation binding_;
        cxx::basic_type_info implementation_;
    };
} // namespace lux::render
