#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/meta/MetaCompat.hpp>

#include <algorithm>
#include <exception>
#include <utility>

namespace lux::flowforge
{
    namespace
    {
        auto structural(graph::EGraphTopologyError code, NodeId node = {}, PinId pin = {}) noexcept
        {
            return cxx::unexpected(VFlowGraphFailure{graph::GraphTopologyFailure{code, node, pin}});
        }

        auto semantic(FlowForgeFailure error, NodeId node) noexcept
        {
            error.node_id = node.value;
            return cxx::unexpected(VFlowGraphFailure{std::move(error)});
        }

        bool compatible(const FlowPinPayload& from, const FlowPinPayload& to) noexcept
        {
            const bool is_execution = from.role == EFlowPinRole::EXECUTION && to.role == EFlowPinRole::EXECUTION;
            const bool is_data = from.role == EFlowPinRole::DATA && to.role == EFlowPinRole::DATA;
            const bool has_types = is_data && from.type && to.type;
            return is_execution || (has_types && meta::canInitialize(to.type, from.type));
        }

        std::uint8_t fanCap(const FlowPinDeclaration& pin) noexcept
        {
            const bool is_single = pin.role == EFlowPinRole::EXECUTION ? pin.direction == graph::EPinDirection::OUTPUT
                                                                       : pin.direction == graph::EPinDirection::INPUT;
            return is_single ? 1U : graph::kUnlimitedFan;
        }

        graph::LinkRecord orientLink(const graph::GraphTopology& topology, PinId first, PinId second) noexcept
        {
            const auto* pin = topology.findPin(first);
            const bool is_input = pin && pin->direction == graph::EPinDirection::INPUT;
            return is_input ? graph::LinkRecord{second, first} : graph::LinkRecord{first, second};
        }
    } // namespace

    FlowGraph::FlowGraph() noexcept = default;

    FlowGraph::~FlowGraph() = default;

    FlowGraph::FlowGraph(FlowGraph&&) noexcept = default;

    FlowGraph& FlowGraph::operator=(FlowGraph&& other) noexcept
    {
        if (this != &other)
        {
            // Keep the entire old allocation alive until all swaps finish. Its pins are destroyed
            // before their metadata providers; map move assignment alone would violate that order.
            FlowGraph previous(std::move(other));
            variables_.swap(previous.variables_);
            exports_.swap(previous.exports_);
            std::swap(next_var_id_, previous.next_var_id_);
            nodes_.swap(previous.nodes_);
            pins_.swap(previous.pins_);
            std::swap(topology_, previous.topology_);
            std::swap(layout_, previous.layout_);
        }
        return *this;
    }

    const FlowNode* FlowGraph::node(NodeId id) const noexcept
    {
        const auto found = nodes_.find(id);
        return found == nodes_.end() ? nullptr : &found->second;
    }

    const FlowPinPayload* FlowGraph::pin(PinId id) const noexcept
    {
        const auto found = pins_.find(id);
        return found == pins_.end() ? nullptr : &found->second;
    }

    FlowPinPayload* FlowGraph::pin(PinId id) noexcept
    {
        return const_cast<FlowPinPayload*>(std::as_const(*this).pin(id));
    }

    PinId FlowGraph::pinId(NodeId owner, graph::PinSemanticId semantic) const noexcept
    {
        for (const auto& record : topology_.pins())
        {
            const bool matches = record.owner == owner && record.semantic == semantic;
            if (matches)
            {
                return record.id;
            }
        }
        return {};
    }

    FlowGraphResult<NodeId> FlowGraph::addNode(FlowNode value) noexcept
    {
        return addNodeWithId({}, std::move(value));
    }

    FlowGraphResult<NodeId> FlowGraph::addNodeWithId(
        NodeId id,
        FlowNode value,
        std::span<const FlowPinEntry> pins
    ) noexcept
    {
        const FlowNodeEntry entry{id, &value, pins};
        FlowGraphChange change;
        change.insert = {&entry, 1};
        auto edit = FlowGraphEdit::prepare(*this, change);
        if (!edit)
        {
            return cxx::unexpected(std::move(edit.error()));
        }
        const auto assigned = edit->insertedNodes().front().id;
        edit->commit();
        return assigned;
    }

    FlowGraphResult<FlowNodeSnapshot> FlowGraph::extractNode(NodeId id) noexcept
    {
        FlowGraphChange change;
        change.erase = {&id, 1};
        auto edit = FlowGraphEdit::prepare(*this, change);
        if (!edit)
        {
            return cxx::unexpected(std::move(edit.error()));
        }
        edit->commit();
        auto removed = edit->takeRemoved();
        return std::move(removed.front());
    }

    FlowGraphResult<void> FlowGraph::removeNode(NodeId id) noexcept
    {
        auto removed = extractNode(id);
        if (!removed)
        {
            return cxx::unexpected(std::move(removed.error()));
        }
        return {};
    }

    FlowGraphResult<void> FlowGraph::connect(PinId from, PinId to) noexcept
    {
        const auto link = orientLink(topology_, from, to);
        FlowGraphChange change;
        change.connect = {&link, 1};
        auto edit = FlowGraphEdit::prepare(*this, change);
        if (!edit)
        {
            return cxx::unexpected(std::move(edit.error()));
        }
        edit->commit();
        return {};
    }

    FlowGraphResult<void> FlowGraph::disconnect(PinId from, PinId to) noexcept
    {
        const auto link = orientLink(topology_, from, to);
        FlowGraphChange change;
        change.disconnect = {&link, 1};
        auto edit = FlowGraphEdit::prepare(*this, change);
        if (!edit)
        {
            return cxx::unexpected(std::move(edit.error()));
        }
        edit->commit();
        return {};
    }

    FlowGraphEdit::FlowGraphEdit(FlowGraph& target) noexcept
        : target_(&target), structure_(target.topology_, target.layout_)
    {
    }

    FlowGraphEdit::~FlowGraphEdit() = default;

    FlowGraphEdit::FlowGraphEdit(FlowGraphEdit&& other) noexcept
        : target_(std::exchange(other.target_, nullptr)), structure_(std::move(other.structure_)),
          staged_nodes_(std::move(other.staged_nodes_)), removed_(std::move(other.removed_)),
          staged_pins_(std::move(other.staged_pins_)), inserted_pins_(std::move(other.inserted_pins_)),
          inserted_(std::move(other.inserted_)), committed_(std::exchange(other.committed_, true))
    {
    }

    FlowGraphResult<void> FlowGraphEdit::insert(
        NodeId id,
        FlowNode value,
        std::span<const FlowPinEntry> restored
    ) noexcept
    {
        if (!value.definition)
        {
            return structural(graph::EGraphTopologyError::INVALID_TYPE, id);
        }
        auto declarations = value.definition->describePins(value.payload);
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
        const auto type = value.definition->identity().id;
        if (id.valid())
        {
            auto result = structure_.insertNode({id, type});
            if (!result)
            {
                return cxx::unexpected(VFlowGraphFailure{result.error()});
            }
        }
        else
        {
            auto result = structure_.addNode(type);
            if (!result)
            {
                return cxx::unexpected(VFlowGraphFailure{result.error()});
            }
            id = *result;
        }
        std::vector<FlowPinEntry> entries;
        entries.reserve(declarations->size());
        for (const auto& declaration : *declarations)
        {
            graph::PinRecord record{{}, id, declaration.direction, fanCap(declaration), declaration.semantic};
            FlowPinPayload pin{
                declaration.name,
                declaration.role,
                declaration.type,
                declaration.allow_default,
                declaration.necessary,
                {}
            };
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
                const bool has_owner = found->record.owner == id;
                const bool has_direction = found->record.direction == record.direction;
                const bool has_fan = found->record.fan_cap == record.fan_cap;
                const bool has_role = found->value.role == declaration.role;
                const bool has_type = declaration.role == EFlowPinRole::EXECUTION
                                          ? found->value.type == nullptr
                                          : found->value.type && *found->value.type == *declaration.type;
                const bool is_invalid = !has_owner || !has_direction || !has_fan || !has_role || !has_type;
                if (is_invalid)
                {
                    return structural(graph::EGraphTopologyError::INVALID_TYPE, id, found->record.id);
                }
                auto cloned = found->value.clone();
                if (!cloned)
                {
                    return cxx::unexpected(VFlowGraphFailure{cloned.error()});
                }
                pin = std::move(*cloned);
                record.id = found->record.id;
                if (record.id.valid())
                {
                    auto accepted = structure_.insertPin(record);
                    if (!accepted)
                    {
                        return cxx::unexpected(VFlowGraphFailure{accepted.error()});
                    }
                }
            }
            else if (declaration.initial_value)
            {
                auto initial = declaration.initial_value(value.payload, *declaration.type);
                if (!initial)
                {
                    return semantic(std::move(initial.error()), id);
                }
                const bool is_invalid_default = initial->isValid() && !pin.setDefault(std::move(*initial));
                if (is_invalid_default)
                {
                    return semantic(
                        FlowForgeFailure{
                            EFlowForgeError::GRAPH_INVALID,
                            "pin initializer returned an incompatible value"
                        },
                        id
                    );
                }
            }
            entries.push_back({record, std::move(pin)});
        }
        // All explicit IDs across the whole batch are installed before finishPins issues new IDs.
        staged_nodes_.emplace(id, std::move(value));
        inserted_pins_.push_back(std::move(entries));
        inserted_.push_back({id, {}});
        return {};
    }

    FlowGraphResult<void> FlowGraphEdit::finishPins() noexcept
    {
        for (std::size_t i = 0; i != inserted_pins_.size(); ++i)
        {
            auto& assignment = inserted_[i];
            assignment.pins.reserve(inserted_pins_[i].size());
            for (auto& entry : inserted_pins_[i])
            {
                auto& record = entry.record;
                if (!record.id.valid())
                {
                    auto id = structure_.addPin(record.owner, record.direction, record.fan_cap, record.semantic);
                    if (!id)
                    {
                        return cxx::unexpected(VFlowGraphFailure{id.error()});
                    }
                    record.id = *id;
                }
                assignment.pins.push_back(record);
                staged_pins_.emplace(record.id, std::move(entry.value));
            }
        }
        return {};
    }

    FlowGraphResult<FlowGraphEdit> FlowGraphEdit::prepare(FlowGraph& source, const FlowGraphChange& change) noexcept
    {
        FlowGraphEdit plan(source);
        plan.inserted_.reserve(change.insert.size());
        plan.inserted_pins_.reserve(change.insert.size());
        plan.removed_.reserve(change.erase.size());
        for (const auto& link : change.disconnect)
        {
            auto result = plan.structure_.disconnect(link.from, link.to);
            if (!result)
            {
                return cxx::unexpected(VFlowGraphFailure{result.error()});
            }
        }
        for (const auto id : change.erase)
        {
            if (!source.node(id))
            {
                return structural(graph::EGraphTopologyError::UNKNOWN_NODE, id);
            }
            auto detached = plan.structure_.detachNode(id);
            if (!detached)
            {
                return cxx::unexpected(VFlowGraphFailure{detached.error()});
            }
            FlowNodeSnapshot snapshot;
            snapshot.id = id;
            snapshot.links = std::move(detached->links);
            if (const auto* layout = source.layout_.find(id))
            {
                snapshot.layout = *layout;
            }
            snapshot.pins.reserve(detached->pins.size());
            for (const auto& record : detached->pins)
            {
                if (!source.pin(record.id))
                {
                    return structural(graph::EGraphTopologyError::UNKNOWN_PIN, id, record.id);
                }
                snapshot.pins.push_back({record, {}});
            }
            plan.removed_.push_back(std::move(snapshot));
        }
        std::vector<FlowNode> copies;
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
        std::vector<std::size_t> requested_order(change.insert.size());
        for (const bool explicit_id : {true, false})
        {
            for (std::size_t i = 0; i != change.insert.size(); ++i)
            {
                const auto& entry = change.insert[i];
                if (entry.id.valid() != explicit_id)
                {
                    continue;
                }
                requested_order[i] = plan.inserted_.size();
                auto result = plan.insert(entry.id, std::move(copies[i]), entry.pins);
                if (!result)
                {
                    return cxx::unexpected(std::move(result.error()));
                }
            }
        }
        auto assigned = plan.finishPins();
        if (!assigned)
        {
            return cxx::unexpected(std::move(assigned.error()));
        }
        std::vector<FlowNodeAssignment> ordered;
        ordered.reserve(requested_order.size());
        for (const auto index : requested_order)
        {
            ordered.push_back(std::move(plan.inserted_[index]));
        }
        plan.inserted_ = std::move(ordered);
        const bool has_node_changes = !change.insert.empty() || !change.erase.empty();
        if (has_node_changes)
        {
            const auto find_node = [&](NodeId id) noexcept -> const FlowNode*
            {
                if (!plan.structure_.topology().findNode(id))
                {
                    return nullptr;
                }
                const auto staged = plan.staged_nodes_.find(id);
                return staged == plan.staged_nodes_.end() ? source.node(id) : &staged->second;
            };
            const auto variable_type = [&](std::uint64_t id) noexcept -> const meta::RefType*
            {
                const auto* variable = source.findVariable(id);
                return variable ? variable->type : nullptr;
            };
            const FlowReferenceView references{find_node, variable_type};
            for (const auto& record : plan.structure_.topology().nodes())
            {
                const auto* value = find_node(record.id);
                const bool has_definition = value && value->definition;
                if (!has_definition)
                {
                    return structural(graph::EGraphTopologyError::INVALID_TYPE, record.id);
                }
                auto validated = value->definition->validateReferences(value->payload, references);
                if (!validated)
                {
                    return semantic(std::move(validated.error()), record.id);
                }
            }
        }
        for (const auto& link : change.connect)
        {
            auto connected = plan.structure_.connect(link.from, link.to);
            if (!connected)
            {
                return cxx::unexpected(VFlowGraphFailure{connected.error()});
            }
            const auto find = [&](PinId id) noexcept -> const FlowPinPayload*
            {
                const auto staged = plan.staged_pins_.find(id);
                return staged == plan.staged_pins_.end() ? source.pin(id) : &staged->second;
            };
            const auto* from = find(link.from);
            const auto* to = find(link.to);
            const auto* from_record = plan.structure_.topology().findPin(link.from);
            const auto* to_record = plan.structure_.topology().findPin(link.to);
            const bool has_values = from && to;
            const bool has_records = from_record && to_record;
            const bool is_same_node = has_records && from_record->owner == to_record->owner;
            const bool is_valid = has_values && has_records && !is_same_node && compatible(*from, *to);
            if (!is_valid)
            {
                return structural(graph::EGraphTopologyError::INVALID_TYPE, {}, link.to);
            }
        }
        for (const auto& entry : change.place)
        {
            auto result = plan.structure_.place(entry.node, entry.layout);
            if (!result)
            {
                return cxx::unexpected(VFlowGraphFailure{result.error()});
            }
        }
        for (const auto id : change.unplace)
        {
            auto result = plan.structure_.unplace(id);
            if (!result)
            {
                return cxx::unexpected(VFlowGraphFailure{result.error()});
            }
        }
        plan.reserveCommit();
        return plan;
    }

    void FlowGraphEdit::reserveCommit() noexcept
    {
        target_->pins_.reserve(target_->pins_.size() + staged_pins_.size());
    }

    std::span<const FlowNodeAssignment> FlowGraphEdit::insertedNodes() const noexcept
    {
        return inserted_;
    }

    FlowGraphResult<void> FlowGraphEdit::place(NodeId id, graph::GraphNodeLayout layout) noexcept
    {
        const bool is_invalid_edit = committed_ || !target_;
        if (is_invalid_edit)
        {
            return structural(graph::EGraphTopologyError::INVALID_ID, id);
        }
        auto result = structure_.place(id, layout);
        if (!result)
        {
            return cxx::unexpected(VFlowGraphFailure{result.error()});
        }
        return {};
    }

    void FlowGraphEdit::commit() noexcept
    {
        const bool is_invalid_edit = committed_ || !target_;
        if (is_invalid_edit)
        {
            std::terminate();
        }
        structure_.commit();
        for (auto& snapshot : removed_)
        {
            auto node = target_->nodes_.extract(snapshot.id);
            snapshot.value = std::move(node.mapped());
            for (auto& pin : snapshot.pins)
            {
                auto value = target_->pins_.extract(pin.record.id);
                pin.value = std::move(value.mapped());
            }
        }
        const auto transfer = [](auto& from, auto& to) noexcept
        {
            while (!from.empty())
            {
                auto inserted = to.insert(from.extract(from.begin()));
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

    std::vector<FlowNodeSnapshot> FlowGraphEdit::takeRemoved() noexcept
    {
        if (!committed_)
        {
            std::terminate();
        }
        return std::move(removed_);
    }
} // namespace lux::flowforge
