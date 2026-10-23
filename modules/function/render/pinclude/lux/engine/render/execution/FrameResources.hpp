#pragma once

#include <lux/engine/render/execution/HistoryState.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/PhysicalResourcePlan.hpp>

namespace lux::render::detail
{
    // These are physical GPU objects, independent of the Context's subscriptions and CPU recipes.
    // Stable descriptor banks are never rewritten while their prior submission is in flight.
    struct RenderGraphInstance::FrameResources final
    {
        void release(const nri::CoreInterface&) noexcept;

        std::vector<Binding> bindings;
        // Changes only when an imported physical identity or range changes, never for its contents.
        std::uint64_t imported_revision = 0;
        std::vector<Pass> passes;

        struct DrawList final
        {
            nri::Buffer* vertices = nullptr;
            nri::Buffer* indices = nullptr;
            std::uint64_t vertex_capacity = 0, index_capacity = 0;
            std::vector<IndexedDrawRange> ranges;
            std::vector<std::byte> parameters;
        };

        std::vector<DrawList> draw_lists;
        std::vector<std::byte> parameters;
        std::vector<std::uint8_t> parameter_dirty;
        std::vector<std::uint32_t> parameter_updates;
        std::vector<std::uint8_t> enabled_regions;
        std::vector<std::uint8_t> history_valid;
        std::uint32_t enabled_pass_count = 0;
        RasterViewport viewport;
        std::vector<std::uint32_t> changed_resources;
        std::size_t unavailable_buffers = 0;
        std::vector<nri::Buffer*> buffers;
        std::vector<nri::Texture*> images;
        std::vector<nri::Descriptor*> views;
        // Stable addresses of output attachment fields, built once after all Pass arrays are fixed.
        std::vector<nri::Descriptor**> output_views;

        struct MemoryAllocation final
        {
            nri::Memory* memory;
            std::uint64_t bytes;
        };

        std::vector<MemoryAllocation> memories;
        std::vector<MemoryPlacement> memory_bindings;
        nri::DescriptorPool* descriptor_pool = nullptr;
        std::vector<nri::TextureBarrierDesc> final_images;
        std::vector<nri::BufferBarrierDesc> final_buffers;
    };

} // namespace lux::render::detail
