#pragma once

#include <lux/engine/render/graph/RasterViewport.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>

namespace lux::render
{
    enum class EAttachmentLoad : std::uint8_t
    {
        DISCARD,
        CLEAR,
        LOAD
    };

    enum class EAttachmentStore : std::uint8_t
    {
        DISCARD,
        STORE
    };

    struct RasterAttachment final
    {
        ResourceAccess access;
        EAttachmentLoad load = EAttachmentLoad::CLEAR;
        EAttachmentStore store = EAttachmentStore::STORE;
        std::array<float, 4> clear{};
        float clear_depth = 1;
        std::optional<ResourceAccess> resolve;
    };

    struct RasterScope
    {
        [[nodiscard]] bool hasAttachments() const noexcept
        {
            return !colors.empty() || depth.has_value();
        }

        std::vector<RasterAttachment> colors;
        std::optional<RasterAttachment> depth;
        // Per-pass rectangle in its attachment; absent uses the instance's normalized viewport.
        std::optional<RasterViewport> viewport;
    };

} // namespace lux::render
