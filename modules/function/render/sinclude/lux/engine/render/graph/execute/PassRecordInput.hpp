#pragma once

#include <NRI.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <span>

namespace lux::render::detail
{
    struct BindingState;
    struct RasterDrawList;
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
        std::span<nri::Descriptor* const> private_views, history_views, clear_views;
        std::span<const nri::AttachmentDesc> colors;
        nri::AttachmentDesc depth{};
        nri::DescriptorSet* private_descriptors = nullptr;
        std::span<const nri::TextureDataLayoutDesc> copy_layouts;
        std::array<std::uint32_t, 3> dispatch_groups{};
        std::array<nri::DescriptorSet*, 3> shared_descriptors{};
        nri::Viewport viewport{};
        nri::Rect scissor{};
        nri::DepthBiasDesc depth_bias{};
        const detail::RasterDrawList* draws = nullptr;
        detail::BindingState* bindings = nullptr;
        bool begin_rendering = false, end_rendering = false, reset_viewport = false;
    };

    struct PassRecordInput final
    {
        std::span<const PassResourceInput> resources;
        std::span<const std::byte> parameters;
        const PassNativeInput* native = nullptr;
    };
} // namespace lux::render
