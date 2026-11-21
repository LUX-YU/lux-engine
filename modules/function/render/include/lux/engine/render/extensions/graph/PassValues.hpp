#pragma once

#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>

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

    enum class EConditionSource : std::uint8_t
    {
        INPUT,
        HISTORY_VALID
    };

    struct PassCondition final
    {
        EConditionSource source = EConditionSource::INPUT;
        // INPUT: declared condition index. HISTORY_VALID: the Previous resource of a declared pair.
        std::uint32_t input = 0;
        bool expected = true;
        bool operator==(const PassCondition&) const noexcept = default;
    };
} // namespace lux::render
