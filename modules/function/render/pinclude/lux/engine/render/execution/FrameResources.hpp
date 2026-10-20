#pragma once

#include <lux/engine/render/execution/ExecutionContext.hpp>

namespace lux::render::detail
{
    // These are physical GPU objects, independent of the Context's subscriptions and CPU recipes.
    // Stable descriptor banks are never rewritten while their prior submission is in flight.
    struct ExecutionContext::FrameResources final
    {
        void release(const nri::CoreInterface&) noexcept;

        std::vector<Binding> bindings;
        std::vector<Pass> passes;
        std::vector<std::uint32_t> changed_resources;
        std::size_t unavailable_buffers = 0;
        std::vector<nri::Buffer*> buffers;
        std::vector<nri::Texture*> images;
        std::vector<nri::Descriptor*> views;
        nri::DescriptorPool* descriptor_pool = nullptr;
    };

} // namespace lux::render::detail
