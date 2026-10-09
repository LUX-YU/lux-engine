#include <lux/engine/material/graph/MaterialGraph.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace lux::material
{
    namespace
    {
        auto structural(graph::EGraphTopologyError code, NodeId node = {}, PinId pin = {}) noexcept
        {
            return cxx::unexpected(VMaterialGraphFailure{graph::GraphTopologyFailure{code, node, pin}});
        }

        auto semantic(MaterialCompileFailure error, NodeId node) noexcept
        {
            error.node_id = node;
            return cxx::unexpected(VMaterialGraphFailure{std::move(error)});
        }

        bool validPin(const MaterialPinPayload& pin) noexcept
        {
            const bool is_valid_type = pin.type >= EValueType::FLOAT && pin.type <= EValueType::VEC4;
            const bool is_finite =
                std::ranges::all_of(pin.constant, [](float value) noexcept { return std::isfinite(value); });
            return is_valid_type && is_finite;
        }

        bool convertible(EValueType source, EValueType destination) noexcept
        {
            const bool is_scalar = source == EValueType::FLOAT;
            return source == destination || source > destination || is_scalar;
        }
    } // namespace

    MaterialNodeResult<MaterialNode> MaterialNode::clone() const noexcept
    {
        if (!definition)
        {
            return cxx::unexpected(
                MaterialCompileFailure{EMaterialCompileError::INVALID_GRAPH, "node has no registered definition"}
            );
        }
        auto copied = payload.clone();
        if (!copied)
        {
            return cxx::unexpected(std::move(copied.error()));
        }
        return MaterialNode{definition, name, std::move(*copied)};
    }

    MaterialGraph::MaterialGraph() noexcept = default;

    MaterialGraph::~MaterialGraph() = default;

    MaterialGraph::MaterialGraph(MaterialGraph&&) noexcept = default;

    MaterialGraph& MaterialGraph::operator=(MaterialGraph&&) noexcept = default;

    MaterialGraphResult<MaterialGraph> MaterialGraph::clone() const noexcept
    {
        MaterialGraph result;
        result.shading_model = shading_model;
        result.texture_slots = texture_slots;
        result.param_slots = param_slots;
        result.render_state = render_state;
        result.topology_ = topology_;
        result.layout_ = layout_;
        result.pins_ = pins_;
        result.nodes_.reserve(nodes_.size());
        for (const auto& [id, value] : nodes_)
        {
            auto copied = value.clone();
            if (!copied)
            {
                return semantic(std::move(copied.error()), id);
            }
            result.nodes_.emplace(id, std::move(*copied));
        }
        return result;
    }

    MaterialGraphResult<NodeId> MaterialGraph::addNode(MaterialNode node) noexcept
    {
        return addNodeWithId({}, std::move(node));
    }

    MaterialGraphResult<NodeId> MaterialGraph::addNodeWithId(
        NodeId id,
        MaterialNode node,
        std::span<const MaterialPinEntry> pins
    ) noexcept
    {
        MaterialGraphEdit edit(*this);
        edit.inserted_.reserve(1);
        edit.inserted_pins_.reserve(1);
        auto inserted = edit.insert(id, std::move(node), pins);
        if (!inserted)
        {
            return cxx::unexpected(std::move(inserted.error()));
        }
        const auto assigned = edit.inserted_.front().id;
        edit.reserveCommit();
        edit.commit();
        return assigned;
    }

    MaterialGraphResult<MaterialNodeSnapshot> MaterialGraph::extractNode(NodeId id) noexcept
    {
        const auto found = nodes_.find(id);
        if (found == nodes_.end())
        {
            return structural(graph::EGraphTopologyError::UNKNOWN_NODE, id);
        }
        graph::GraphEdit edit(topology_, layout_);
        auto detached = edit.detachNode(id);
        if (!detached)
        {
            return cxx::unexpected(VMaterialGraphFailure{detached.error()});
        }
        MaterialNodeSnapshot result;
        result.id = id;
        result.links = std::move(detached->links);
        if (const auto* position = layout_.find(id))
        {
            result.layout = *position;
        }
        result.pins.reserve(detached->pins.size());
        for (const auto& record : detached->pins)
        {
            const auto* value = pin(record.id);
            if (!value)
            {
                return structural(graph::EGraphTopologyError::UNKNOWN_PIN, id, record.id);
            }
            result.pins.push_back({record, *value});
        }
        // All preparation has succeeded. Moving owners and erasing plain metadata cannot call extensions.
        edit.commit();
        result.value = std::move(found->second);
        nodes_.erase(found);
        for (const auto& record : result.pins)
        {
            pins_.erase(record.record.id);
        }
        return result;
    }

    MaterialGraphResult<void> MaterialGraph::removeNode(NodeId id) noexcept
    {
        MaterialGraphChange change;
        change.erase = {&id, 1};
        auto edit = MaterialGraphEdit::prepare(*this, change);
        if (!edit)
        {
            return cxx::unexpected(std::move(edit.error()));
        }
        edit->commit();
        return {};
    }

    const MaterialNode* MaterialGraph::node(NodeId id) const noexcept
    {
        const auto found = nodes_.find(id);
        return found == nodes_.end() ? nullptr : &found->second;
    }

    const MaterialPinPayload* MaterialGraph::pin(PinId id) const noexcept
    {
        const auto found = pins_.find(id);
        return found == pins_.end() ? nullptr : &found->second;
    }

    MaterialPinPayload* MaterialGraph::pin(PinId id) noexcept
    {
        return const_cast<MaterialPinPayload*>(std::as_const(*this).pin(id));
    }

    PinId MaterialGraph::pinId(NodeId node, graph::PinSemanticId semantic) const noexcept
    {
        for (const auto& record : topology_.pins())
        {
            if (record.owner == node && record.semantic == semantic)
            {
                return record.id;
            }
        }
        return {};
    }

    MaterialGraphResult<void> MaterialGraph::connect(PinId from, PinId to) noexcept
    {
        const graph::LinkRecord link{from, to};
        MaterialGraphChange change;
        change.connect = {&link, 1};
        auto edit = MaterialGraphEdit::prepare(*this, change);
        if (!edit)
        {
            return cxx::unexpected(std::move(edit.error()));
        }
        edit->commit();
        return {};
    }

    MaterialGraphResult<void> MaterialGraph::disconnect(PinId input) noexcept
    {
        const auto link = topology_.incoming(input);
        if (!link)
        {
            return structural(graph::EGraphTopologyError::UNKNOWN_LINK, {}, input);
        }
        MaterialGraphChange change;
        change.disconnect = {&*link, 1};
        auto edit = MaterialGraphEdit::prepare(*this, change);
        if (!edit)
        {
            return cxx::unexpected(std::move(edit.error()));
        }
        edit->commit();
        return {};
    }

    const graph::GraphTopology& MaterialGraph::topology() const noexcept
    {
        return topology_;
    }

    const graph::GraphLayout& MaterialGraph::layout() const noexcept
    {
        return layout_;
    }

    MaterialGraphEdit::MaterialGraphEdit(MaterialGraph& graph) noexcept
        : target_(&graph), structure_(graph.topology_, graph.layout_)
    {
    }

    MaterialGraphEdit::~MaterialGraphEdit() = default;

    MaterialGraphEdit::MaterialGraphEdit(MaterialGraphEdit&& other) noexcept
        : target_(std::exchange(other.target_, nullptr)), structure_(std::move(other.structure_)),
          staged_nodes_(std::move(other.staged_nodes_)), staged_pins_(std::move(other.staged_pins_)),
          retired_nodes_(std::move(other.retired_nodes_)), retired_pins_(std::move(other.retired_pins_)),
          erase_nodes_(std::move(other.erase_nodes_)), erase_pins_(std::move(other.erase_pins_)),
          inserted_pins_(std::move(other.inserted_pins_)), inserted_(std::move(other.inserted_)),
          committed_(std::exchange(other.committed_, true))
    {
    }

    MaterialGraphResult<void> MaterialGraphEdit::insert(
        NodeId id,
        MaterialNode node,
        std::span<const MaterialPinEntry> restored
    ) noexcept
    {
        if (!node.definition)
        {
            return structural(graph::EGraphTopologyError::INVALID_TYPE, id);
        }
        auto declarations = node.definition->describePins(node.payload);
        if (!declarations)
        {
            return semantic(std::move(declarations.error()), id);
        }
        const bool has_restore = !restored.empty();
        const bool is_invalid_restore = has_restore && (!id.valid() || restored.size() != declarations->size());
        if (is_invalid_restore)
        {
            return structural(graph::EGraphTopologyError::INVALID_SEMANTIC, id);
        }
        const auto type = node.definition->identity().id;
        if (id.valid())
        {
            auto accepted = structure_.insertNode({id, type});
            if (!accepted)
            {
                return cxx::unexpected(VMaterialGraphFailure{accepted.error()});
            }
        }
        else
        {
            auto assigned = structure_.addNode(type);
            if (!assigned)
            {
                return cxx::unexpected(VMaterialGraphFailure{assigned.error()});
            }
            id = *assigned;
        }
        std::vector<MaterialPinEntry> entries;
        entries.reserve(declarations->size());
        for (const auto& declaration : *declarations)
        {
            const auto fan = declaration.direction == graph::EPinDirection::INPUT ? 1U : graph::kUnlimitedFan;
            graph::PinRecord
                record{{}, id, declaration.direction, static_cast<std::uint8_t>(fan), declaration.semantic};
            MaterialPinPayload value{declaration.name, declaration.type, declaration.default_value};
            if (has_restore)
            {
                const auto found = std::ranges::find(
                    restored,
                    declaration.semantic,
                    [](const auto& entry) noexcept { return entry.record.semantic; }
                );
                if (found == restored.end())
                {
                    return structural(graph::EGraphTopologyError::INVALID_SEMANTIC, id);
                }
                const bool is_wrong_owner = found->record.owner != id;
                const bool is_wrong_direction = found->record.direction != record.direction;
                const bool is_wrong_fan = found->record.fan_cap != record.fan_cap;
                const bool is_invalid_record = is_wrong_owner || is_wrong_direction || is_wrong_fan;
                if (is_invalid_record)
                {
                    return structural(graph::EGraphTopologyError::INVALID_SEMANTIC, id, found->record.id);
                }
                record.id = found->record.id;
                value = found->value;
                auto accepted = structure_.insertPin(record);
                if (!accepted)
                {
                    return cxx::unexpected(VMaterialGraphFailure{accepted.error()});
                }
            }
            else
            {
                auto assigned = structure_.addPin(id, record.direction, record.fan_cap, record.semantic);
                if (!assigned)
                {
                    return cxx::unexpected(VMaterialGraphFailure{assigned.error()});
                }
                record.id = *assigned;
            }
            if (!validPin(value))
            {
                return structural(graph::EGraphTopologyError::INVALID_TYPE, id, record.id);
            }
            entries.push_back({record, value});
            staged_pins_.emplace(record.id, std::move(value));
        }
        auto stored = staged_nodes_.emplace(id, std::move(node));
        if (!stored.second)
        {
            return structural(graph::EGraphTopologyError::DUPLICATE_NODE, id);
        }
        inserted_pins_.push_back(std::move(entries));
        inserted_.push_back({id, &stored.first->second, inserted_pins_.back()});
        return {};
    }

    MaterialGraphResult<MaterialGraphEdit> MaterialGraphEdit::prepare(
        MaterialGraph& source,
        const MaterialGraphChange& change
    ) noexcept
    {
        MaterialGraphEdit result(source);
        result.inserted_.reserve(change.insert.size());
        result.inserted_pins_.reserve(change.insert.size());
        for (const auto& link : change.disconnect)
        {
            auto removed = result.structure_.disconnect(link.from, link.to);
            if (!removed)
            {
                return cxx::unexpected(VMaterialGraphFailure{removed.error()});
            }
        }
        for (const auto id : change.erase)
        {
            if (!source.node(id))
            {
                return structural(graph::EGraphTopologyError::UNKNOWN_NODE, id);
            }
            auto removed = result.structure_.detachNode(id);
            if (!removed)
            {
                return cxx::unexpected(VMaterialGraphFailure{removed.error()});
            }
            result.erase_nodes_.push_back(id);
            for (const auto& pin : removed->pins)
            {
                result.erase_pins_.push_back(pin.id);
            }
        }
        std::vector<MaterialNode> copies;
        copies.reserve(change.insert.size());
        for (const auto& entry : change.insert)
        {
            if (!entry.value)
            {
                return structural(graph::EGraphTopologyError::INVALID_TYPE, entry.id);
            }
            auto copy = entry.value->clone();
            if (!copy)
            {
                return semantic(std::move(copy.error()), entry.id);
            }
            copies.push_back(std::move(*copy));
        }
        // Restore explicit identities before issuing any new ones. Report insertions in request order.
        std::vector<MaterialNodeEntry> ordered(change.insert.size());
        // Restored pins precede fresh pins, including nodes with an explicit id but a fresh schema.
        for (unsigned phase = 0; phase != 3; ++phase)
        {
            for (std::size_t index = 0; index != change.insert.size(); ++index)
            {
                const auto& entry = change.insert[index];
                const unsigned entry_phase = !entry.pins.empty() ? 0U : entry.id.valid() ? 1U : 2U;
                if (entry_phase != phase)
                {
                    continue;
                }
                auto inserted = result.insert(entry.id, std::move(copies[index]), entry.pins);
                if (!inserted)
                {
                    return cxx::unexpected(std::move(inserted.error()));
                }
                ordered[index] = result.inserted_.back();
            }
        }
        result.inserted_ = std::move(ordered);
        for (const auto& link : change.connect)
        {
            // Topology validates identity/direction/fan before semantic payload lookup.
            auto connected = result.structure_.connect(link.from, link.to);
            if (!connected)
            {
                return cxx::unexpected(VMaterialGraphFailure{connected.error()});
            }
            const auto find = [&](PinId id) noexcept -> const MaterialPinPayload*
            {
                const auto staged = result.staged_pins_.find(id);
                return staged == result.staged_pins_.end() ? source.pin(id) : &staged->second;
            };
            const auto* from = find(link.from);
            const auto* to = find(link.to);
            const bool has_payloads = from && to;
            const bool has_valid_values = has_payloads && validPin(*from) && validPin(*to);
            const bool has_compatible_types = has_valid_values && convertible(from->type, to->type);
            if (!has_compatible_types)
            {
                return structural(graph::EGraphTopologyError::INVALID_TYPE, {}, link.from);
            }
        }
        for (const auto& entry : change.place)
        {
            auto placed = result.structure_.place(entry.node, entry.layout);
            if (!placed)
            {
                return cxx::unexpected(VMaterialGraphFailure{placed.error()});
            }
        }
        for (const auto id : change.unplace)
        {
            auto removed = result.structure_.unplace(id);
            if (!removed)
            {
                return cxx::unexpected(VMaterialGraphFailure{removed.error()});
            }
        }
        result.reserveCommit();
        return result;
    }

    void MaterialGraphEdit::reserveCommit() noexcept
    {
        target_->nodes_.reserve(target_->nodes_.size() + staged_nodes_.size());
        target_->pins_.reserve(target_->pins_.size() + staged_pins_.size());
        retired_nodes_.reserve(erase_nodes_.size());
        retired_pins_.reserve(erase_pins_.size());
    }

    std::span<const MaterialNodeEntry> MaterialGraphEdit::insertedNodes() const noexcept
    {
        return inserted_;
    }

    MaterialGraphResult<void> MaterialGraphEdit::place(NodeId id, graph::GraphNodeLayout value) noexcept
    {
        if (committed_ || !target_)
        {
            return structural(graph::EGraphTopologyError::INVALID_ID, id);
        }
        auto placed = structure_.place(id, value);
        if (!placed)
        {
            return cxx::unexpected(VMaterialGraphFailure{placed.error()});
        }
        return {};
    }

    void MaterialGraphEdit::commit() noexcept
    {
        if (committed_ || !target_)
        {
            std::terminate();
        }
        structure_.commit();
        for (const auto id : erase_nodes_)
        {
            retired_nodes_.push_back(target_->nodes_.extract(id));
        }
        for (const auto id : erase_pins_)
        {
            retired_pins_.push_back(target_->pins_.extract(id));
        }
        const auto transfer = [](auto& source, auto& target) noexcept
        {
            while (!source.empty())
            {
                auto inserted = target.insert(source.extract(source.begin()));
                if (!inserted.inserted)
                {
                    std::terminate();
                }
            }
        };
        transfer(staged_nodes_, target_->nodes_);
        transfer(staged_pins_, target_->pins_);
        committed_ = true;
    }
} // namespace lux::material
