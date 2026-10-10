#pragma once

#include <cstdint>
#include <span>
#include <vector>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Identity.hpp>

namespace lux::render
{
    struct GraphResourceTag;
    struct GraphPassTag;
    using GraphResourceId = cxx::StrongId<GraphResourceTag, std::uint32_t, 0>;
    using GraphPassId = cxx::StrongId<GraphPassTag, std::uint32_t, 0>;

    enum class EGraphResourceKind { BUFFER, IMAGE };
    enum class EGraphResourceOrigin { TRANSIENT, IMPORTED };
    enum class EGraphAccess { READ, WRITE, READ_WRITE };
    enum class EGraphUsage { TRANSFER, SHADER, COLOR_ATTACHMENT, DEPTH_ATTACHMENT, VERTEX, INDEX, UNIFORM, PRESENT };

    // IDs are one-based declaration positions, scoped to this definition. They
    // are neither runtime handles nor identities across unrelated definitions.
    struct GraphResource
    {
        EGraphResourceKind kind{EGraphResourceKind::BUFFER};
        EGraphResourceOrigin origin{EGraphResourceOrigin::TRANSIENT};
        RenderTargetSemanticId target_semantic{};

        bool operator==(const GraphResource&) const noexcept = default;
    };

    struct GraphResourceUse
    {
        GraphResourceId resource;
        EGraphAccess access{EGraphAccess::READ};
        EGraphUsage usage{EGraphUsage::SHADER};

        bool operator==(const GraphResourceUse&) const noexcept = default;
    };

    struct GraphPass
    {
        std::vector<GraphResourceUse> uses;

        bool operator==(const GraphPass&) const noexcept = default;
    };

    struct GraphDependency
    {
        GraphPassId before;
        GraphPassId after;

        bool operator==(const GraphDependency&) const noexcept = default;
    };

    inline constexpr auto kGraphInvalidResource = error::errorId("lux.render.graph.invalid_resource");
    inline constexpr auto kGraphInvalidUse = error::errorId("lux.render.graph.invalid_use");
    inline constexpr auto kGraphInvalidDependency = error::errorId("lux.render.graph.invalid_dependency");
    inline constexpr auto kGraphCycle = error::errorId("lux.render.graph.cycle");
    inline constexpr auto kGraphMissingProducer = error::errorId("lux.render.graph.missing_producer");
    inline constexpr auto kGraphInvalidBinding = error::errorId("lux.render.graph.invalid_binding");

    [[nodiscard]] std::span<const error::ErrorDescriptor> renderGraphErrorDescriptors() noexcept;

    // Owns declarations, not execution objects. Construction validates all IDs
    // and usage combinations; compile separately proves scheduling legality.
    class RenderGraphDefinition
    {
    public:
        [[nodiscard]] static RenderResult<RenderGraphDefinition> create(
            std::vector<GraphResource> resources,
            std::vector<GraphPass> passes,
            std::vector<GraphDependency> dependencies = {}
        ) noexcept;

        [[nodiscard]] std::span<const GraphResource> resources() const noexcept { return resources_; }

        [[nodiscard]] std::span<const GraphPass> passes() const noexcept { return passes_; }

        [[nodiscard]] std::span<const GraphDependency> dependencies() const noexcept { return dependencies_; }

        // Cold topology comparison: exact value equality, no hash collisions,
        // simulation revision, frame time, backing tokens or dynamic offsets.
        bool operator==(const RenderGraphDefinition&) const noexcept = default;

    private:
        RenderGraphDefinition(
            std::vector<GraphResource> resources,
            std::vector<GraphPass> passes,
            std::vector<GraphDependency> dependencies
        ) noexcept;

        std::vector<GraphResource> resources_;
        std::vector<GraphPass> passes_;
        std::vector<GraphDependency> dependencies_;
    };
}
