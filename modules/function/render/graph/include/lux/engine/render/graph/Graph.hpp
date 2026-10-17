#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/core/Ids.hpp>

namespace lux::render
{
    struct GraphBufferDesc final
    {
        std::uint64_t size = 0;
        bool operator==(const GraphBufferDesc&) const noexcept = default;
    };

    struct GraphImageDesc final
    {
        std::uint32_t width = 1;
        std::uint32_t height = 1;
        std::uint32_t mips = 1;
        std::uint32_t layers = 1;
        rdesc::ETextureFormat format = rdesc::ETextureFormat::RGBA8_UNORM;
        std::uint32_t samples = 1;
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

    // monostate explicitly means the complete declared resource.
    using VGraphRange = std::variant<std::monostate, GraphBufferRange, GraphImageRange>;

    enum class EGraphOrigin : std::uint8_t { TRANSIENT, IMPORTED, HISTORY_PREVIOUS, HISTORY_CURRENT };
    enum class EGraphPassKind : std::uint8_t { GRAPHICS, COMPUTE, TRANSFER };
    enum class EGraphAccess : std::uint8_t { READ, WRITE, READ_WRITE, ATTACHMENT_LOAD };
    // Orthogonal to logical version access. UNSPECIFIED remains legal for neutral analysis;
    // a Native compiler rejects it when it needs an execution recipe.
    enum class EGraphUse : std::uint8_t
    {
        UNSPECIFIED, COLOR_ATTACHMENT, TRANSFER_SOURCE, TRANSFER_DESTINATION,
        SAMPLED, STORAGE_READ, STORAGE_WRITE, STORAGE_READ_WRITE, VERTEX, INDEX, INDIRECT, DEPTH_ATTACHMENT
    };

    struct GraphResource final
    {
        std::string key;
        VGraphResourceDesc description = GraphBufferDesc{};
        EGraphOrigin origin = EGraphOrigin::TRANSIENT;
        std::vector<VGraphRange> initial_ranges;
    };

    struct GraphVersion final
    {
        GraphResourceId resource;
        std::uint32_t number = 1; // 0 = explicitly available initial content
        bool operator==(const GraphVersion&) const noexcept = default;
    };

    struct GraphResourceUse final
    {
        GraphVersion version;
        VGraphRange range;
        EGraphAccess access = EGraphAccess::READ;
        std::uint32_t input_version = 0; // READ_WRITE/LOAD reads this predecessor
        // Conditional write: preserve an earlier version of the SAME resource.
        // READ of unavailable previous history: select a compatible other source.
        std::optional<GraphVersion> fallback;
        EGraphUse usage = EGraphUse::UNSPECIFIED;
    };

    struct GraphPass final
    {
        std::string key;
        EGraphPassKind kind = EGraphPassKind::COMPUTE;
        std::vector<GraphResourceUse> uses;
        bool side_effect = false;
        bool conditional = false;
    };

    struct GraphDependency final
    {
        GraphPassId before;
        GraphPassId after;
        bool operator==(const GraphDependency&) const noexcept = default;
    };

    struct GraphOutput final
    {
        GraphVersion version;
        VGraphRange range;
        // Present only for an actual final color output; G1 contract is reused.
        std::optional<TargetDesc> target;
    };

    class RenderGraphBuilder;

    class RenderGraphDefinition final
    {
    public:
        [[nodiscard]] std::span<const GraphResource> resources() const noexcept { return resources_; }
        [[nodiscard]] std::span<const GraphPass> passes() const noexcept { return passes_; }
        [[nodiscard]] std::span<const GraphDependency> dependencies() const noexcept { return dependencies_; }
        [[nodiscard]] std::span<const GraphOutput> outputs() const noexcept { return outputs_; }

    private:
        friend class RenderGraphBuilder;
        std::vector<GraphResource> resources_;
        std::vector<GraphPass> passes_;
        std::vector<GraphDependency> dependencies_;
        std::vector<GraphOutput> outputs_;
    };

    // Error args: {pass index (or invalid), resource index (or invalid), detail}.
    inline constexpr auto kGraphUnknownResource = error::errorId("lux.render.graph.unknown_resource");
    inline constexpr auto kGraphUnknownPass = error::errorId("lux.render.graph.unknown_pass");
    inline constexpr auto kGraphInvalidDescriptor = error::errorId("lux.render.graph.invalid_descriptor");
    inline constexpr auto kGraphInvalidRange = error::errorId("lux.render.graph.invalid_range");
    inline constexpr auto kGraphDuplicateKey = error::errorId("lux.render.graph.duplicate_key");
    inline constexpr auto kGraphConflictingOutput = error::errorId("lux.render.graph.conflicting_output");
    inline constexpr auto kGraphUndefinedRead = error::errorId("lux.render.graph.undefined_read");
    inline constexpr auto kGraphAmbiguousProducer = error::errorId("lux.render.graph.ambiguous_producer");
    inline constexpr auto kGraphDependencyCycle = error::errorId("lux.render.graph.dependency_cycle");
    inline constexpr auto kGraphMissingFallback = error::errorId("lux.render.graph.missing_fallback");
    inline constexpr auto kGraphMissingInitial = error::errorId("lux.render.graph.missing_initial");

    [[nodiscard]] std::span<const error::ErrorDescriptor> renderGraphErrorDescriptors() noexcept;
}
