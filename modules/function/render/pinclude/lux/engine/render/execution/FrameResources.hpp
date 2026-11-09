#pragma once

#include <lux/engine/render/execution/DrawListResources.hpp>
#include <lux/engine/render/execution/HistoryState.hpp>
#include <lux/engine/render/execution/RasterDraws.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/PhysicalResourcePlan.hpp>
#include <unordered_map>

namespace lux::render::detail
{
    // These are physical GPU objects, independent of the Context's subscriptions and CPU recipes.
    // Stable descriptor banks are never rewritten while their prior submission is in flight.
    struct RenderGraphInstance::FrameResources final
    {
        void release(const nri::CoreInterface&) noexcept;
        void invalidateImportedBindings() noexcept;
        [[nodiscard]] static RenderResult<void> addDescriptors(nri::DescriptorPoolDesc&, const PassBindings&) noexcept;

        struct RasterBinding final
        {
            nri::DescriptorSet* set = nullptr;
            std::uint64_t binding_key = 0;
            std::vector<nri::Descriptor*> views;
            std::vector<ResourceInput> inputs;
            const PassBindings* bindings = nullptr;
        };

        using RasterBindingIndices = std::unordered_map<std::uint64_t, std::uint32_t>;

        std::vector<RasterBinding> raster_bindings;
        std::unordered_map<const PassBindings*, RasterBindingIndices> raster_binding_indices;
        nri::DescriptorPoolDesc descriptor_capacity{}, descriptor_reserved{};

        std::vector<Binding> bindings;
        // Changes when a potentially shared imported identity/range changes. Private presentation rotation
        // updates bindings and barriers separately; resource contents do not invalidate dependencies.
        std::uint64_t imported_revision = 0;
        std::vector<Pass> passes;
        // Per FIF and compiled submission. These handles never borrow the temporary upload pool.
        std::vector<std::array<nri::CommandAllocator*, 2>> command_allocators;
        std::vector<std::array<nri::CommandBuffer*, 2>> command_buffers;
        std::vector<std::uint64_t> recording_revisions, recorded_revisions;
        std::uint64_t shared_binding_revision = 0;

        std::vector<DrawListResources> draw_lists;
        std::vector<RasterDrawList> raster_draws;
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

        struct LayeredImage final
        {
            GraphResourceId resource;
            std::vector<nri::Descriptor*> views; // Non-owning index into this frame's owned view collection.
        };

        std::vector<LayeredImage> layered_images;
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
