#pragma once

#include <lux/engine/render/graph/BufferBoundary.hpp>
#include <span>

namespace lux::render::detail
{
    struct FinalBufferWrite final
    {
        GraphBufferRange bytes;
        std::uint32_t batch, condition_region;
    };

    // The compiler sorts by byte offset. Skip disabled writes without counting across a gap.
    [[nodiscard]] constexpr bool
    bufferWritesCover(GraphBufferRange range, std::span<const FinalBufferWrite> writes, auto enabled) noexcept
    {
        if (!range.size) return false;
        const auto end = range.offset + range.size;
        auto covered = range.offset;
        for (const auto& write : writes)
        {
            if (write.bytes.offset > covered) break;
            const auto write_end = write.bytes.offset + write.bytes.size;
            if (write_end <= covered || !enabled(write.condition_region)) continue;
            if (write_end >= end) return true;
            covered = write_end;
        }
        return false;
    }

    struct FinalBufferExport final
    {
        GraphBufferRange bytes;
        std::optional<PassCondition> when;
        // Empty once the compiler proves unconditional coverage. Unwritable exports are omitted.
        std::span<const FinalBufferWrite> conditional_writes;
    };

    // A device Effect owns the contiguous use/export tables behind these ranges.
    struct ImportedResourceSlot final
    {
        GraphResourceId resource;
        std::optional<GraphBufferRange> output_range;
        BufferBoundary boundary;
        bool unconditional_access = false;
        std::span<const std::uint32_t> binding_regions;
        std::span<const FinalBufferWrite> writes;
        std::span<const FinalBufferExport> exports;
    };
}
