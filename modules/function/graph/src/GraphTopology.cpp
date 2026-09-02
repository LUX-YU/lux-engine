#include <lux/engine/function/graph/GraphTopology.hpp>

#include <algorithm>
#include <new>
#include <utility>

namespace lux::graph
{
    namespace
    {
        template<class Record, class Id, class Projection>
        [[nodiscard]] const Record* findRecord(std::span<const Record> records, Id id, Projection projection) noexcept
        {
            const auto found = std::ranges::find_if(records, [id, projection](const Record& record) noexcept {
                return projection(record) == id;
            });
            return found == records.end() ? nullptr : std::addressof(*found);
        }

        void sortRecords(std::vector<NodeRecord>& nodes, std::vector<PinRecord>& pins, std::vector<LinkRecord>& links)
        {
            std::ranges::sort(nodes, {}, [](const NodeRecord& value) noexcept { return value.id; });
            std::ranges::sort(pins, {}, [](const PinRecord& value) noexcept { return value.id; });
            std::ranges::sort(links, [](const LinkRecord& left, const LinkRecord& right) noexcept {
                return left.from < right.from || (left.from == right.from && left.to < right.to);
            });
        }
    } // namespace

    GraphTopologyFailure GraphTopology::failure(
        EGraphTopologyError code,
        NodeId node,
        PinId pin,
        PinId related_pin
    ) noexcept
    {
        return GraphTopologyFailure{code, node, pin, related_pin};
    }

    lux::cxx::expected<NodeId, GraphTopologyFailure> GraphTopology::addNode(NodeTypeId type) noexcept
    {
        if (!type.valid())
            return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_TYPE));
        if (next_node_id_ == 0U)
            return lux::cxx::unexpected(failure(EGraphTopologyError::ID_EXHAUSTED));
        const NodeId id{next_node_id_};
        if (auto inserted = insertNode(NodeRecord{id, type}); !inserted)
            return lux::cxx::unexpected(inserted.error());
        return id;
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphTopology::insertNode(NodeRecord node) noexcept
    {
        if (!node.id.valid())
            return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_ID, node.id));
        if (!node.type.valid())
            return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_TYPE, node.id));
        if (findNode(node.id) != nullptr)
            return lux::cxx::unexpected(failure(EGraphTopologyError::DUPLICATE_NODE, node.id));
        try
        {
            nodes_.push_back(node);
            std::ranges::sort(nodes_, {}, [](const NodeRecord& value) noexcept { return value.id; });
            advanceNodeId(node.id);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EGraphTopologyError::ALLOCATION_FAILURE, node.id));
        }
    }

    lux::cxx::expected<DetachedNode, GraphTopologyFailure> GraphTopology::detachNode(NodeId node) noexcept
    {
        const auto* record = findNode(node);
        if (record == nullptr)
            return lux::cxx::unexpected(failure(EGraphTopologyError::UNKNOWN_NODE, node));
        try
        {
            DetachedNode detached;
            detached.node = *record;
            for (const auto& pin : pins_)
                if (pin.owner == node)
                    detached.pins.push_back(pin);
            for (const auto& link : links_)
            {
                const auto* from = findPin(link.from);
                const auto* to = findPin(link.to);
                if ((from != nullptr && from->owner == node) || (to != nullptr && to->owner == node))
                    detached.links.push_back(link);
            }

            std::erase_if(links_, [&](const LinkRecord& link) {
                return std::ranges::any_of(detached.links, [&](const LinkRecord& value) { return value == link; });
            });
            std::erase_if(pins_, [node](const PinRecord& pin) { return pin.owner == node; });
            std::erase_if(nodes_, [node](const NodeRecord& value) { return value.id == node; });
            return detached;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EGraphTopologyError::ALLOCATION_FAILURE, node));
        }
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphTopology::restoreNode(DetachedNode node) noexcept
    {
        try
        {
            auto nodes = nodes_;
            auto pins = pins_;
            auto links = links_;
            nodes.push_back(node.node);
            pins.insert(pins.end(), node.pins.begin(), node.pins.end());
            links.insert(links.end(), node.links.begin(), node.links.end());
            sortRecords(nodes, pins, links);
            if (auto valid = validateCombined(nodes, pins, links); !valid)
                return valid;
            nodes_.swap(nodes);
            pins_.swap(pins);
            links_.swap(links);
            advanceNodeId(node.node.id);
            for (const auto& pin : node.pins)
                advancePinId(pin.id);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EGraphTopologyError::ALLOCATION_FAILURE, node.node.id));
        }
    }

    lux::cxx::expected<PinId, GraphTopologyFailure> GraphTopology::addPin(
        NodeId owner,
        EPinDirection direction,
        std::uint8_t fan_cap,
        PinSemanticId semantic
    ) noexcept
    {
        if (next_pin_id_ == 0U)
            return lux::cxx::unexpected(failure(EGraphTopologyError::ID_EXHAUSTED, owner));
        const PinId id{next_pin_id_};
        if (auto inserted = insertPin(PinRecord{id, owner, direction, fan_cap, semantic}); !inserted)
            return lux::cxx::unexpected(inserted.error());
        return id;
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphTopology::insertPin(PinRecord pin) noexcept
    {
        if (!pin.id.valid() || !pin.owner.valid())
            return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_ID, pin.owner, pin.id));
        const bool is_invalid_direction = pin.direction != EPinDirection::INPUT &&
            pin.direction != EPinDirection::OUTPUT;
        if (is_invalid_direction)
            return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_DIRECTION, pin.owner, pin.id));
        if (pin.fan_cap == 0U)
            return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_FAN_CAP, pin.owner, pin.id));
        if (!pin.semantic.valid())
            return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_SEMANTIC, pin.owner, pin.id));
        if (findNode(pin.owner) == nullptr)
            return lux::cxx::unexpected(failure(EGraphTopologyError::UNKNOWN_NODE, pin.owner, pin.id));
        if (findPin(pin.id) != nullptr)
            return lux::cxx::unexpected(failure(EGraphTopologyError::DUPLICATE_PIN, pin.owner, pin.id));
        try
        {
            pins_.push_back(pin);
            std::ranges::sort(pins_, {}, [](const PinRecord& value) noexcept { return value.id; });
            advancePinId(pin.id);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EGraphTopologyError::ALLOCATION_FAILURE, pin.owner, pin.id));
        }
    }

    lux::cxx::expected<DetachedPin, GraphTopologyFailure> GraphTopology::detachPin(PinId pin) noexcept
    {
        const auto* record = findPin(pin);
        if (record == nullptr)
            return lux::cxx::unexpected(failure(EGraphTopologyError::UNKNOWN_PIN, {}, pin));
        try
        {
            DetachedPin detached;
            detached.pin = *record;
            for (const auto& link : links_)
                if (link.from == pin || link.to == pin)
                    detached.links.push_back(link);
            std::erase_if(links_, [pin](const LinkRecord& link) { return link.from == pin || link.to == pin; });
            std::erase_if(pins_, [pin](const PinRecord& value) { return value.id == pin; });
            return detached;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EGraphTopologyError::ALLOCATION_FAILURE, {}, pin));
        }
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphTopology::restorePin(DetachedPin pin) noexcept
    {
        try
        {
            auto nodes = nodes_;
            auto pins = pins_;
            auto links = links_;
            pins.push_back(pin.pin);
            links.insert(links.end(), pin.links.begin(), pin.links.end());
            sortRecords(nodes, pins, links);
            if (auto valid = validateCombined(nodes, pins, links); !valid)
                return valid;
            pins_.swap(pins);
            links_.swap(links);
            advancePinId(pin.pin.id);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EGraphTopologyError::ALLOCATION_FAILURE, pin.pin.owner, pin.pin.id));
        }
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphTopology::connect(PinId from, PinId to) noexcept
    {
        const auto* from_pin = findPin(from);
        const auto* to_pin = findPin(to);
        if (from_pin == nullptr)
            return lux::cxx::unexpected(failure(EGraphTopologyError::UNKNOWN_PIN, {}, from, to));
        if (to_pin == nullptr)
            return lux::cxx::unexpected(failure(EGraphTopologyError::UNKNOWN_PIN, {}, to, from));
        if (from_pin->direction != EPinDirection::OUTPUT || to_pin->direction != EPinDirection::INPUT)
            return lux::cxx::unexpected(failure(EGraphTopologyError::DIRECTION_MISMATCH, {}, from, to));
        if (findLink(from, to) != nullptr)
            return lux::cxx::unexpected(failure(EGraphTopologyError::DUPLICATE_LINK, {}, from, to));
        const bool from_full = from_pin->fan_cap != kUnlimitedFan && linkCount(from) >= from_pin->fan_cap;
        const bool to_full = to_pin->fan_cap != kUnlimitedFan && linkCount(to) >= to_pin->fan_cap;
        if (from_full || to_full)
            return lux::cxx::unexpected(failure(EGraphTopologyError::FAN_CAP_EXCEEDED, {}, from, to));
        try
        {
            links_.push_back(LinkRecord{from, to});
            std::ranges::sort(links_, [](const LinkRecord& left, const LinkRecord& right) noexcept {
                return left.from < right.from || (left.from == right.from && left.to < right.to);
            });
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EGraphTopologyError::ALLOCATION_FAILURE, {}, from, to));
        }
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphTopology::disconnect(PinId from, PinId to) noexcept
    {
        const auto found = std::ranges::find(links_, LinkRecord{from, to});
        if (found == links_.end())
            return lux::cxx::unexpected(failure(EGraphTopologyError::UNKNOWN_LINK, {}, from, to));
        links_.erase(found);
        return {};
    }

    const NodeRecord* GraphTopology::findNode(NodeId node) const noexcept
    {
        return findRecord<NodeRecord>(nodes_, node, [](const NodeRecord& value) noexcept { return value.id; });
    }

    const PinRecord* GraphTopology::findPin(PinId pin) const noexcept
    {
        return findRecord<PinRecord>(pins_, pin, [](const PinRecord& value) noexcept { return value.id; });
    }

    const LinkRecord* GraphTopology::findLink(PinId from, PinId to) const noexcept
    {
        const auto found = std::ranges::find(links_, LinkRecord{from, to});
        return found == links_.end() ? nullptr : std::addressof(*found);
    }

    std::optional<LinkRecord> GraphTopology::incoming(PinId input) const noexcept
    {
        const auto found = std::ranges::find_if(links_, [input](const LinkRecord& link) { return link.to == input; });
        return found == links_.end() ? std::optional<LinkRecord>{} : std::optional<LinkRecord>{*found};
    }

    std::size_t GraphTopology::linkCount(PinId pin) const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(links_, [pin](const LinkRecord& link) {
            return link.from == pin || link.to == pin;
        }));
    }

    std::span<const NodeRecord> GraphTopology::nodes() const noexcept
    {
        return nodes_;
    }

    std::span<const PinRecord> GraphTopology::pins() const noexcept
    {
        return pins_;
    }

    std::span<const LinkRecord> GraphTopology::links() const noexcept
    {
        return links_;
    }

    lux::cxx::expected<void, GraphTopologyFailure> GraphTopology::validateCombined(
        std::span<const NodeRecord> nodes,
        std::span<const PinRecord> pins,
        std::span<const LinkRecord> links
    ) const noexcept
    {
        for (std::size_t index{}; index < nodes.size(); ++index)
        {
            if (!nodes[index].id.valid())
                return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_ID, nodes[index].id));
            if (!nodes[index].type.valid())
                return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_TYPE, nodes[index].id));
            if (findRecord<NodeRecord>(nodes.subspan(index + 1U), nodes[index].id, [](const NodeRecord& value) {
                    return value.id;
                }) != nullptr)
            {
                return lux::cxx::unexpected(failure(EGraphTopologyError::DUPLICATE_NODE, nodes[index].id));
            }
        }
        for (std::size_t index{}; index < pins.size(); ++index)
        {
            const auto& pin = pins[index];
            if (!pin.id.valid() || !pin.owner.valid())
                return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_ID, pin.owner, pin.id));
            if (findRecord<NodeRecord>(nodes, pin.owner, [](const NodeRecord& value) { return value.id; }) == nullptr)
                return lux::cxx::unexpected(failure(EGraphTopologyError::UNKNOWN_NODE, pin.owner, pin.id));
            if (pin.fan_cap == 0U)
                return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_FAN_CAP, pin.owner, pin.id));
            const bool is_invalid_direction = pin.direction != EPinDirection::INPUT &&
                pin.direction != EPinDirection::OUTPUT;
            if (is_invalid_direction)
                return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_DIRECTION, pin.owner, pin.id));
            if (!pin.semantic.valid())
                return lux::cxx::unexpected(failure(EGraphTopologyError::INVALID_SEMANTIC, pin.owner, pin.id));
            if (findRecord<PinRecord>(pins.subspan(index + 1U), pin.id, [](const PinRecord& value) {
                    return value.id;
                }) != nullptr)
            {
                return lux::cxx::unexpected(failure(EGraphTopologyError::DUPLICATE_PIN, pin.owner, pin.id));
            }
        }
        for (std::size_t index{}; index < links.size(); ++index)
        {
            const auto& link = links[index];
            const auto* from = findRecord<PinRecord>(pins, link.from, [](const PinRecord& value) { return value.id; });
            const auto* to = findRecord<PinRecord>(pins, link.to, [](const PinRecord& value) { return value.id; });
            if (from == nullptr || to == nullptr)
                return lux::cxx::unexpected(failure(EGraphTopologyError::UNKNOWN_PIN, {}, link.from, link.to));
            if (from->direction != EPinDirection::OUTPUT || to->direction != EPinDirection::INPUT)
                return lux::cxx::unexpected(failure(EGraphTopologyError::DIRECTION_MISMATCH, {}, link.from, link.to));
            if (std::ranges::find(links.subspan(index + 1U), link) != links.end())
                return lux::cxx::unexpected(failure(EGraphTopologyError::DUPLICATE_LINK, {}, link.from, link.to));
            const auto from_count = std::ranges::count_if(links, [&](const LinkRecord& value) {
                return value.from == link.from || value.to == link.from;
            });
            const auto to_count = std::ranges::count_if(links, [&](const LinkRecord& value) {
                return value.from == link.to || value.to == link.to;
            });
            const bool from_full = from->fan_cap != kUnlimitedFan && from_count > from->fan_cap;
            const bool to_full = to->fan_cap != kUnlimitedFan && to_count > to->fan_cap;
            if (from_full || to_full)
                return lux::cxx::unexpected(failure(EGraphTopologyError::FAN_CAP_EXCEEDED, {}, link.from, link.to));
        }
        return {};
    }

    void GraphTopology::advanceNodeId(NodeId id) noexcept
    {
        if (id.value >= next_node_id_)
            next_node_id_ = id.value + 1U;
    }

    void GraphTopology::advancePinId(PinId id) noexcept
    {
        if (id.value >= next_pin_id_)
            next_pin_id_ = id.value + 1U;
    }
} // namespace lux::graph
