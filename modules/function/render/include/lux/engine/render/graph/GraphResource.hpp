#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <lux/engine/description/Image.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/graph/Ids.hpp>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lux::render
{
    struct GraphBufferDesc final
    {
        std::uint64_t size = 0;
        bool operator==(const GraphBufferDesc&) const noexcept = default;
    };

    enum class EImageExtentSource : std::uint8_t
    {
        FIXED,
        TARGET,
        // Dimensions come from the imported physical image; format, mips and layers stay declared.
        IMPORTED
    };

    struct ImageExtent final
    {
        EImageExtentSource source = EImageExtentSource::FIXED;
        std::uint32_t numerator = 1;
        std::uint32_t denominator = 1;
        std::uint32_t alignment = 1;
        bool operator==(const ImageExtent&) const noexcept = default;
    };

    struct GraphImageDesc final
    {
        std::uint32_t width = 1;
        std::uint32_t height = 1;
        std::uint32_t mips = 1;
        std::uint32_t layers = 1;
        rdesc::ETextureFormat format = rdesc::ETextureFormat::RGBA8_UNORM;
        std::uint32_t samples = 1;
        ImageExtent extent;
        // A Feature prepares the actual array layer count in the acquired FIF slot.
        // Such images use whole-resource accesses and never share transient alias allocations.
        bool runtime_layers = false;
        bool operator==(const GraphImageDesc&) const noexcept = default;
    };

    using VGraphResourceDesc = std::variant<GraphBufferDesc, GraphImageDesc>;

    struct GraphBufferRange final
    {
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        bool operator==(const GraphBufferRange&) const noexcept = default;
    };

    // Aspect bits are the existing rdesc::textureAspectMask contract (color=1,
    // depth=2, stencil=4); intervals are half-open and never silently clamped.
    struct GraphImageRange final
    {
        std::uint32_t aspects = 1;
        std::uint32_t first_mip = 0;
        std::uint32_t mip_count = 1;
        std::uint32_t first_layer = 0;
        std::uint32_t layer_count = 1;
        bool operator==(const GraphImageRange&) const noexcept = default;
    };

    struct ImageCopyRegion final
    {
        std::uint32_t x = 0, y = 0, width = 0, height = 0;
    };

    // monostate explicitly means the complete declared resource.
    using VGraphRange = std::variant<std::monostate, GraphBufferRange, GraphImageRange>;

    enum class EGraphOrigin : std::uint8_t
    {
        TRANSIENT,
        IMPORTED,
        HISTORY_PREVIOUS,
        HISTORY_CURRENT,
        FRAME_BUFFER // Per-FIF buffer sized by an Effect preparation and defined by GPU work.
    };
    enum class EGraphPassKind : std::uint8_t
    {
        GRAPHICS,
        COMPUTE,
        TRANSFER
    };
    enum class EGraphAccess : std::uint8_t
    {
        READ,
        WRITE,
        READ_WRITE,
        ATTACHMENT_LOAD
    };
    // Orthogonal to logical version access. UNSPECIFIED remains legal for neutral analysis;
    // a Native compiler rejects it when it needs an execution recipe.
    enum class EGraphUse : std::uint8_t
    {
        UNSPECIFIED,
        COLOR_ATTACHMENT,
        TRANSFER_SOURCE,
        TRANSFER_DESTINATION,
        SAMPLED,
        STORAGE_READ,
        STORAGE_WRITE,
        STORAGE_READ_WRITE,
        VERTEX,
        INDEX,
        INDIRECT,
        DEPTH_ATTACHMENT,
        UNIFORM_READ,
        PRESENT // Terminal state of a single-sample exported target image.
    };

    struct GraphResource final
    {
        std::string key;
        VGraphResourceDesc description = GraphBufferDesc{};
        EGraphOrigin origin = EGraphOrigin::TRANSIENT;
        std::vector<VGraphRange> initial_ranges;
        // Imported physical input state. UNSPECIFIED is legal only when no image contents are read.
        EGraphUse initial_use = EGraphUse::UNSPECIFIED;
        // Cold Job installation claims this half-open byte range from a named final Buffer's owner.
        std::optional<GraphBufferRange> output_range;
    };

    struct ResourceVersion final
    {
        GraphResourceId resource;
        std::uint32_t number = 1; // 0 = explicitly available initial content
        // Authored choice of two versions. Normalization resolves this before a pass enters the executable.
        std::uint32_t selection = kGraphInvalidIndex;
        bool operator==(const ResourceVersion&) const noexcept = default;
    };

    // Initial content is recorded before the first use, not inferred from an invalid history sample.
    struct HistoryInitialValue final
    {
        std::array<float, 4> color{};
        float depth = 1;
        std::uint32_t buffer = 0;
    };

    struct HistoryDeclaration final
    {
        VGraphResourceDesc description;
        HistoryInitialValue initial{};
        EGraphUse canonical_use = EGraphUse::STORAGE_READ;
        std::uint32_t distance = 1;
    };

    enum class EConditionSource : std::uint8_t
    {
        INPUT,
        HISTORY_VALID,
        SAME_BINDING
    };

    struct BindingPredicate final
    {
        GraphResourceId left, right;
        bool operator==(const BindingPredicate&) const noexcept = default;
    };

    struct PassCondition final
    {
        EConditionSource source = EConditionSource::INPUT;
        // INPUT and SAME_BINDING index their respective tables. HISTORY_VALID names a Previous resource.
        std::uint32_t input = 0;
        bool expected = true;
        bool operator==(const PassCondition&) const noexcept = default;
    };

    struct HistoryDescription final
    {
        GraphResourceId previous;
        GraphResourceId current;
        // Backwards distance in successful publications. A skipped writer keeps the same history.
        std::uint32_t distance = 1;
        HistoryInitialValue initial;
        EGraphUse canonical_use = EGraphUse::STORAGE_READ;
        std::optional<ResourceVersion> publication;
        std::optional<PassCondition> publish_when;
        std::optional<PassCondition> select_primary;
        std::optional<ResourceVersion> alternate_sample;
    };

    struct ResourceAccess final
    {
        ResourceVersion version;
        VGraphRange range;
        EGraphAccess access = EGraphAccess::READ;
        // READ_WRITE/LOAD reads this predecessor in the written range. Unwritten ranges inherit it;
        // inheritance creates dependencies only when a consumer actually observes those ranges.
        std::uint32_t input_version = 0;
        // Conditional write: preserve an earlier version of the SAME resource.
        // READ of unavailable previous history: select a compatible other source.
        std::optional<ResourceVersion> fallback;
        EGraphUse usage = EGraphUse::UNSPECIFIED;
        // Set by typed ingestion before normalization. A whole imported Buffer view follows its actual slice;
        // an explicitly authored byte range remains that exact range.
        bool whole_resource = false;
    };

    struct GraphDependency final
    {
        GraphPassId before;
        GraphPassId after;
        bool operator==(const GraphDependency&) const noexcept = default;
    };

    struct GraphOutputRequirements final
    {
        rdesc::ETextureFormat format = rdesc::ETextureFormat::UNDEFINED;
        std::uint32_t samples = 1;
        // Both zero means Target-relative; otherwise both dimensions specify a fixed output.
        std::uint32_t width = 0, height = 0;
        bool operator==(const GraphOutputRequirements&) const noexcept = default;
    };

    struct GraphExportAlternative final
    {
        PassCondition primary_when;
        ResourceVersion value;
    };

    struct GraphExport final
    {
        ResourceVersion version;
        VGraphRange range;
        // Logical output requirements contain no Runtime Target kind or ownership.
        std::optional<GraphOutputRequirements> target;
        // Requested physical state after execution; absent leaves the final graph use unchanged.
        std::optional<EGraphUse> final_use;
        std::optional<GraphExportAlternative> alternate;
    };

} // namespace lux::render
