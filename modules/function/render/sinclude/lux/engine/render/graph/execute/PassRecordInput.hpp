#pragma once

#include <lux/engine/render/extensions/device/CopyEncoding.hpp>
#include <variant>
#include <array>
#include <cstddef>
#include <cstdint>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <span>

namespace lux::render::detail
{
    struct BindingState;
    struct RasterDrawList;

    struct FrozenBufferFill final
    {
        nri::Buffer* buffer;
        std::uint64_t offset, size;
        nri::Descriptor* storage_view;
        std::uint32_t descriptor, value;
        bool sync_before;
    };

    struct FrozenImageClear final
    {
        nri::AttachmentDesc attachment;
        bool depth;
    };

    using VFrozenTransfer = std::variant<
        BufferCopyRegion, TextureCopyRegion, UploadRegion, ReadbackRegion, FrozenBufferFill, FrozenImageClear
    >;

}

namespace lux::render
{
    struct PassResourceInput final
    {
        nri::Buffer* buffer = nullptr;
        std::uint64_t buffer_offset = 0;
        nri::Texture* image = nullptr;
        std::uint32_t width = 0, height = 0;
        std::uint64_t buffer_size = 0;
        bool history_valid = true;
    };

    struct PassDescriptorBindings final
    {
        nri::PipelineLayout* layout = nullptr;
        std::uint32_t private_set = 0;
        std::array<std::uint32_t, 3> shared_sets{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    };

    // Prepared native views used by the ordinary graph frontends. No owner or directory lookup.
    // The cache is exclusive to this command buffer; all other borrows are read-only FIF slices.
    struct PassNativeInput final
    {
        std::span<const nri::AttachmentDesc> colors;
        nri::AttachmentDesc depth{};
        std::span<const detail::VFrozenTransfer> transfers;
        nri::DispatchDesc dispatch{};
        nri::Buffer* indirect_buffer = nullptr;
        std::uint64_t indirect_offset = 0;
        nri::Viewport viewport{};
        nri::Rect scissor{};
        const detail::RasterDrawList* draws = nullptr;
        detail::BindingState* bindings = nullptr;
    };

    struct PassRecordInput final
    {
        std::span<const PassResourceInput> resources;
        std::span<nri::DescriptorSet* const, 3> shared_descriptors;
        const PassNativeInput* native = nullptr;
    };
} // namespace lux::render

namespace lux::render::detail
{
    void recordCompute(const PassNativeInput&, nri::CommandBuffer&, const nri::CoreInterface&) noexcept;
    void recordTransfers(const PassNativeInput&, nri::CommandBuffer&, const nri::CoreInterface&) noexcept;
}
