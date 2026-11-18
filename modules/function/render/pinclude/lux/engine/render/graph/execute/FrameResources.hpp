#pragma once

#include <lux/engine/render/graph/execute/DrawListResources.hpp>
#include <lux/engine/render/graph/execute/FrameRecordInput.hpp>
#include <lux/engine/render/graph/execute/HistoryState.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <unordered_map>
#include <atomic>
#include <exception>
#include <utility>

namespace lux::render::detail
{
    // These are physical GPU objects, independent of the Context's subscriptions and CPU recipes.
    // Stable descriptor banks are never rewritten while their prior submission is in flight.
    struct RenderGraphInstance::FrameResources final
    {
        FrameResources() noexcept = default;
        FrameResources(FrameResources&&) noexcept = default;
        FrameResources& operator=(FrameResources&&) noexcept = default;
        FrameResources(const FrameResources&) = delete;
        FrameResources& operator=(const FrameResources&) = delete;

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

        std::vector<PassResourceInput> record_resources;
        std::vector<RecordPass> record_passes;
        std::vector<RecordHistoryInitialization> history_initializations;
        bool record_ready = false;

        std::vector<Binding> bindings;
        // Changes when a potentially shared imported identity/range changes. Private presentation rotation
        // updates bindings and barriers separately; resource contents do not invalidate dependencies.
        std::uint64_t imported_revision = 0;
        std::vector<Pass> passes;
        // Each compiled batch owns its allocators, buffers and cache version in this FIF slice.
        // A recording lease protects the whole batch; distinct batches never share an allocator or scratch.
        struct CommandBatch final
        {
            std::array<nri::CommandAllocator*, 2> allocators{};
            std::array<nri::CommandBuffer*, 2> commands{};
            std::uint64_t revision = 1, recorded_revision = 0;
            std::atomic<bool> recording{false};

            CommandBatch() noexcept = default;
            CommandBatch(const CommandBatch&) = delete;
            CommandBatch& operator=(const CommandBatch&) = delete;
            CommandBatch& operator=(CommandBatch&&) = delete;

            CommandBatch(CommandBatch&& other) noexcept
            {
                // Only cold construction may relocate a batch. Published batches keep stable addresses.
                if (other.recording.load(std::memory_order_acquire)) std::terminate();
                allocators = std::exchange(other.allocators, {});
                commands = std::exchange(other.commands, {});
                revision = other.revision;
                recorded_revision = other.recorded_revision;
            }
        };

        std::vector<CommandBatch> command_batches;
        std::uint64_t shared_binding_revision = 0;
        std::array<nri::DescriptorSet*, 3> shared_descriptors{};
        std::array<std::uint32_t, 3> shared_descriptor_counts{};

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
