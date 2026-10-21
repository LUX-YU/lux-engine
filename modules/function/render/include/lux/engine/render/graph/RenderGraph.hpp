#pragma once

#include <lux/engine/render/graph/Work.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    struct GraphPass final
    {
        std::string key;
        EGraphPassKind kind = EGraphPassKind::COMPUTE;
        std::vector<ResourceAccess> uses;
        VPassWork work;
        bool side_effect = false;
        std::optional<PassCondition> condition;
    };

    class RenderGraphBuilder;

    class RenderGraph final
    {
    public:
        [[nodiscard]] std::span<const GraphResource> resources() const noexcept
        {
            return resources_;
        }

        [[nodiscard]] std::span<const GraphPass> passes() const noexcept
        {
            return passes_;
        }

        [[nodiscard]] std::span<const GraphDependency> dependencies() const noexcept
        {
            return dependencies_;
        }

        [[nodiscard]] std::span<const GraphExport> outputs() const noexcept
        {
            return outputs_;
        }

        [[nodiscard]] std::span<const rdesc::SpirvArtifact> programs() const noexcept
        {
            return programs_;
        }

        [[nodiscard]] std::span<const HistoryDescription> histories() const noexcept
        {
            return histories_;
        }

        [[nodiscard]] std::span<const std::string> conditions() const noexcept
        {
            return conditions_;
        }

    private:
        friend class RenderGraphBuilder;
        std::vector<rdesc::SpirvArtifact> programs_;
        std::vector<GraphResource> resources_;
        std::vector<GraphPass> passes_;
        std::vector<GraphDependency> dependencies_;
        std::vector<GraphExport> outputs_;
        std::vector<HistoryDescription> histories_;
        std::vector<std::string> conditions_;
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

    [[nodiscard]] LUX_RENDER_PUBLIC std::span<const error::ErrorDescriptor> renderGraphErrorDescriptors() noexcept;
} // namespace lux::render
