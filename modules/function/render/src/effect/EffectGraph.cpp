#include <lux/engine/render/effect/EffectGraph.hpp>
#include <algorithm>
#include <utility>

namespace lux::render
{
    const EffectNode* EffectGraph::findNode(graph::NodeId node) const noexcept
    {
        const auto found = nodes_.find(node);
        return found == nodes_.end() ? nullptr : &found->second;
    }

    graph::PinId EffectGraph::pinId(graph::NodeId node, graph::PinSemanticId semantic) const noexcept
    {
        const auto pins = topology_.pins();
        const auto found = std::ranges::find_if(pins, [=](const graph::PinRecord& pin) noexcept
        {
            return pin.owner == node && pin.semantic == semantic;
        });
        return found == pins.end() ? graph::PinId{} : found->id;
    }

    EffectGraphEdit::EffectGraphEdit(EffectGraph& target) noexcept
        : target_(&target), graph_edit_(target.topology_, target.layout_),
          nodes_(target.nodes_), outputs_(target.outputs_)
    {
    }

    RenderError EffectGraphEdit::fail() noexcept
    {
        failed_ = true;
        return RenderError{kCapabilityDeclaration};
    }

    RenderResult<graph::NodeId> EffectGraphEdit::addNode(
        const CapabilityDefinition& capability,
        ImplementationSelection implementation,
        std::string arguments
    ) noexcept
    {
        if (!writable()) return cxx::unexpected(fail());
        const auto& identity = capability.identity;
        const bool is_invalid_identity = !identity.id.valid() || identity.version == 0 ||
            identity.canonical_name.empty() || identity.id != graph::nodeTypeId(identity.canonical_name);
        if (is_invalid_identity) return cxx::unexpected(fail());
        auto added = graph_edit_.addNode(identity.id);
        if (!added) return cxx::unexpected(fail());
        for (const auto& declaration : capability.ports)
        {
            auto pin = graph_edit_.addPin(*added, declaration.direction, declaration.fan_cap, declaration.semantic);
            if (!pin) return cxx::unexpected(fail());
        }
        nodes_.emplace(*added, EffectNode{
            identity.canonical_name, identity.version, std::move(implementation), std::move(arguments)
        });
        return *added;
    }

    RenderResult<void> EffectGraphEdit::removeNode(graph::NodeId node) noexcept
    {
        if (!writable()) return cxx::unexpected(fail());
        if (!nodes_.contains(node)) return cxx::unexpected(fail());
        auto detached = graph_edit_.detachNode(node);
        if (!detached) return cxx::unexpected(fail());
        nodes_.erase(node);
        std::erase_if(outputs_, [&](const EffectOutput& output) noexcept
        {
            return std::ranges::any_of(detached->pins, [&](const graph::PinRecord& pin) noexcept
            {
                return output.source == pin.id;
            });
        });
        return {};
    }

    RenderResult<void> EffectGraphEdit::connect(graph::PinId from, graph::PinId to) noexcept
    {
        if (!writable()) return cxx::unexpected(fail());
        auto connected = graph_edit_.connect(from, to);
        if (!connected) return cxx::unexpected(fail());
        return {};
    }

    RenderResult<void> EffectGraphEdit::disconnect(graph::PinId from, graph::PinId to) noexcept
    {
        if (!writable()) return cxx::unexpected(fail());
        auto disconnected = graph_edit_.disconnect(from, to);
        if (!disconnected) return cxx::unexpected(fail());
        return {};
    }

    RenderResult<void> EffectGraphEdit::setOutput(std::string name, graph::PinId source, bool target) noexcept
    {
        if (!writable()) return cxx::unexpected(fail());
        const auto* pin = graph_edit_.topology().findPin(source);
        if (!pin || name.empty()) return cxx::unexpected(fail());
        if (pin->direction != graph::EPinDirection::OUTPUT) return cxx::unexpected(fail());
        const auto found = std::ranges::find(outputs_, name, &EffectOutput::name);
        EffectOutput output{std::move(name), source, target};
        if (found == outputs_.end()) outputs_.push_back(std::move(output));
        else *found = std::move(output);
        return {};
    }

    RenderResult<void> EffectGraphEdit::removeOutput(std::string_view name) noexcept
    {
        if (!writable()) return cxx::unexpected(fail());
        const auto found = std::ranges::find(outputs_, name, &EffectOutput::name);
        if (found == outputs_.end()) return cxx::unexpected(fail());
        outputs_.erase(found);
        return {};
    }

    RenderResult<void> EffectGraphEdit::setArguments(graph::NodeId node, std::string arguments) noexcept
    {
        if (!writable()) return cxx::unexpected(fail());
        const auto found = nodes_.find(node);
        if (found == nodes_.end()) return cxx::unexpected(fail());
        found->second.arguments = std::move(arguments);
        return {};
    }

    RenderResult<void> EffectGraphEdit::selectImplementation(
        graph::NodeId node, ImplementationSelection implementation
    ) noexcept
    {
        if (!writable()) return cxx::unexpected(fail());
        const auto found = nodes_.find(node);
        if (found == nodes_.end()) return cxx::unexpected(fail());
        // The capability schema and every existing pin ID are unchanged.
        // A newly selected implementation may be infeasible; compilation reports it.
        found->second.implementation = std::move(implementation);
        return {};
    }

    RenderResult<void> EffectGraphEdit::place(graph::NodeId node, graph::GraphNodeLayout layout) noexcept
    {
        if (!writable()) return cxx::unexpected(fail());
        auto placed = graph_edit_.place(node, layout);
        if (!placed) return cxx::unexpected(fail());
        return {};
    }

    graph::PinId EffectGraphEdit::pinId(graph::NodeId node, graph::PinSemanticId semantic) const noexcept
    {
        const auto pins = graph_edit_.topology().pins();
        const auto found = std::ranges::find_if(pins, [=](const graph::PinRecord& pin) noexcept
        {
            return pin.owner == node && pin.semantic == semantic;
        });
        return found == pins.end() ? graph::PinId{} : found->id;
    }

    RenderResult<void> EffectGraphEdit::commit() noexcept
    {
        if (!writable()) return cxx::unexpected(fail());
        graph_edit_.commit();
        target_->nodes_.swap(nodes_);
        target_->outputs_.swap(outputs_);
        target_ = nullptr;
        return {};
    }
}
