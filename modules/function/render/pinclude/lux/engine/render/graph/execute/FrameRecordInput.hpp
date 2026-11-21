#pragma once

#include <NRI.h>
#include <lux/engine/render/graph/execute/PassRecordInput.hpp>
#include <optional>
#include <span>

namespace lux::render::detail
{
    class RenderGraphExecutable;
    class NativeProgramBindings;
    struct RasterDrawList;

    struct RecordPass final
    {
        std::span<const nri::TextureBarrierDesc> images;
        std::span<const nri::BufferBarrierDesc> buffers;
        std::optional<nri::GlobalBarrierDesc> alias_barrier;
        nri::DescriptorSet* private_descriptors = nullptr;
        std::span<const std::byte> parameters;
        PassNativeInput native;
    };

    struct RecordHistoryInitialization final
    {
        nri::TextureBarrierDesc image_before{}, image_after{};
        nri::BufferBarrierDesc buffer_before{}, buffer_after{};
        std::span<nri::Descriptor* const> views;
        nri::ClearValue clear_value{};
        std::uint64_t buffer_size = 0;
        bool depth = false;
    };

    // A synchronous recording borrow of prepared, stable FIF slices. Only the caller owns the
    // mutation window; no owner, scheduler, descriptor directory or History selection is exposed.
    // Parameters and draw bytes already reside in FIF storage, not the client's mutable vectors.
    struct FrameRecordInput final
    {
        const RenderGraphExecutable& plan;
        const NativeProgramBindings& programs;
        std::span<const PassResourceInput> resources;
        std::span<const RecordPass> passes;
        std::span<const RecordHistoryInitialization> history_initializations;
        std::span<const std::uint8_t> enabled_regions;
        std::span<const nri::TextureBarrierDesc> final_images;
        std::span<const nri::BufferBarrierDesc> final_buffers;
        nri::DescriptorPool* descriptor_pool = nullptr;
        std::array<nri::DescriptorSet*, 3> shared_descriptors{};
    };
} // namespace lux::render::detail
