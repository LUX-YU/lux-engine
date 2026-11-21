#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/effect/CapabilityDefinition.hpp>
#include <lux/engine/function/graph/GraphEdit.hpp>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::render
{
    struct EffectNode final
    {
        // The canonical name authenticates the topology's hashed type across catalogs.
        std::string capability_name;
        std::uint32_t capability_version = 1;
        ImplementationSelection implementation;
        std::string arguments;
    };

    struct EffectOutput final
    {
        std::string name;
        graph::PinId source;
        bool target = false;
    };

    class EffectGraphEdit;

    class LUX_RENDER_PUBLIC EffectGraph final
    {
    public:
        [[nodiscard]] const graph::GraphTopology& topology() const & noexcept { return topology_; }
        [[nodiscard]] const graph::GraphLayout& layout() const & noexcept { return layout_; }
        [[nodiscard]] const std::map<graph::NodeId, EffectNode>& nodes() const & noexcept { return nodes_; }
        [[nodiscard]] std::span<const EffectOutput> outputs() const & noexcept { return outputs_; }
        [[nodiscard]] const EffectNode* findNode(graph::NodeId node) const & noexcept;

        const graph::GraphTopology& topology() const && = delete;
        const graph::GraphLayout& layout() const && = delete;
        const std::map<graph::NodeId, EffectNode>& nodes() const && = delete;
        std::span<const EffectOutput> outputs() const && = delete;
        const EffectNode* findNode(graph::NodeId) const && = delete;
        [[nodiscard]] graph::PinId pinId(graph::NodeId node, graph::PinSemanticId semantic) const noexcept;

    private:
        friend class EffectGraphEdit;
        // The sole topology owner. The map has domain payloads, not another graph.
        graph::GraphTopology topology_;
        graph::GraphLayout layout_;
        std::map<graph::NodeId, EffectNode> nodes_;
        std::vector<EffectOutput> outputs_;
    };

    // An exclusive, synchronous draft edit. Domain data uses one candidate copy;
    // topology/layout transaction and monotonic identity retirement are GraphEdit's job.
    // A failed edit cannot commit. Destruction abandons the entire candidate.
    class LUX_RENDER_PUBLIC EffectGraphEdit final
    {
    public:
        explicit EffectGraphEdit(EffectGraph& target) noexcept;
        EffectGraphEdit(const EffectGraphEdit&) = delete;
        EffectGraphEdit& operator=(const EffectGraphEdit&) = delete;
        EffectGraphEdit(EffectGraphEdit&&) = delete;
        EffectGraphEdit& operator=(EffectGraphEdit&&) = delete;

        [[nodiscard]] RenderResult<graph::NodeId> addNode(
            const CapabilityDefinition& capability,
            ImplementationSelection implementation,
            std::string arguments = {}
        ) noexcept;
        [[nodiscard]] RenderResult<void> removeNode(graph::NodeId node) noexcept;
        [[nodiscard]] RenderResult<void> connect(graph::PinId from, graph::PinId to) noexcept;
        [[nodiscard]] RenderResult<void> disconnect(graph::PinId from, graph::PinId to) noexcept;
        [[nodiscard]] RenderResult<void> setOutput(std::string name, graph::PinId source, bool target = false) noexcept;
        [[nodiscard]] RenderResult<void> removeOutput(std::string_view name) noexcept;
        [[nodiscard]] RenderResult<void> setArguments(graph::NodeId node, std::string arguments) noexcept;
        [[nodiscard]] RenderResult<void> selectImplementation(graph::NodeId node, ImplementationSelection implementation) noexcept;
        [[nodiscard]] RenderResult<void> place(graph::NodeId node, graph::GraphNodeLayout layout) noexcept;
        [[nodiscard]] graph::PinId pinId(graph::NodeId node, graph::PinSemanticId semantic) const noexcept;
        [[nodiscard]] RenderResult<void> commit() noexcept;

    private:
        [[nodiscard]] RenderError fail() noexcept;
        [[nodiscard]] bool writable() const noexcept { return target_ && !failed_; }

        EffectGraph* target_;
        graph::GraphEdit graph_edit_;
        std::map<graph::NodeId, EffectNode> nodes_;
        std::vector<EffectOutput> outputs_;
        bool failed_ = false;
    };
}
