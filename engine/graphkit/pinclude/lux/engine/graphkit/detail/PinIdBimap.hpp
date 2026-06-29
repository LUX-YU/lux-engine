#pragma once
// =============================================================================
//  PinIdBimap.hpp (private) — ephemeral imgui-node-editor id <-> GraphRef map.
//
//  imgui-node-editor identifies nodes/pins/links by opaque uintptr ids; the
//  domains identify them by (node-id, side, pin-index) refs. This bimap mints
//  sequential editor ids on demand and resolves them back.
//
//  EAGER-PURGE RULE (v2 dynamism): when a node is removed or reconstructed,
//  purgeNode(id) MUST run before any subsequent add in the same frame —
//  flowforge RECYCLES node ids, so a reused id must never alias a stale
//  mapping; and a pin rebuild invalidates that node's pin indices.
//
//  Ephemeral by design: rebuilt lazily as the canvas paints; rebind clears it
//  wholesale (full-reset contract). Never persisted.
// =============================================================================

#include <cstdint>
#include <unordered_map>
#include <utility>

#include <lux/engine/graphkit/GraphTypes.hpp>

namespace lux::graphkit::detail
{
    class PinIdBimap
    {
    public:
        // ---- mint (stable until purge/clear) --------------------------------
        std::uintptr_t nodeEdId(GraphNodeRef node)
        {
            const auto it = node_to_ed_.find(node.id);
            if (it != node_to_ed_.end())
            {
                return it->second;
            }
            const std::uintptr_t ed = next_++;
            node_to_ed_.emplace(node.id, ed);
            ed_to_node_.emplace(ed, node.id);
            return ed;
        }

        std::uintptr_t pinEdId(GraphPinRef pin)
        {
            const PinKey key = pinKey(pin);
            const auto it = pin_to_ed_.find(key);
            if (it != pin_to_ed_.end())
            {
                return it->second;
            }
            const std::uintptr_t ed = next_++;
            pin_to_ed_.emplace(key, ed);
            ed_to_pin_.emplace(ed, pin);
            return ed;
        }

        std::uintptr_t linkEdId(GraphPinRef from, GraphPinRef to)
        {
            const LinkKey key{ pinKey(from), pinKey(to) };
            const auto it = link_to_ed_.find(key);
            if (it != link_to_ed_.end())
            {
                return it->second;
            }
            const std::uintptr_t ed = next_++;
            link_to_ed_.emplace(key, ed);
            ed_to_link_.emplace(ed, std::pair<GraphPinRef, GraphPinRef>{ from, to });
            return ed;
        }

        // ---- resolve ----------------------------------------------------------
        GraphNodeRef nodeFromEdId(std::uintptr_t ed) const
        {
            const auto it = ed_to_node_.find(ed);
            return it == ed_to_node_.end() ? GraphNodeRef{} : GraphNodeRef{ it->second };
        }

        GraphPinRef pinFromEdId(std::uintptr_t ed) const
        {
            const auto it = ed_to_pin_.find(ed);
            return it == ed_to_pin_.end() ? GraphPinRef{} : it->second;
        }

        bool linkFromEdId(std::uintptr_t ed, GraphPinRef& from, GraphPinRef& to) const
        {
            const auto it = ed_to_link_.find(ed);
            if (it == ed_to_link_.end())
            {
                return false;
            }
            from = it->second.first;
            to   = it->second.second;
            return true;
        }

        // ---- lifecycle ----------------------------------------------------------
        /// Drops every mapping that references @p node (its node id, all its
        /// pins, every link touching it). See the eager-purge rule above.
        void purgeNode(node_id node)
        {
            if (const auto it = node_to_ed_.find(node); it != node_to_ed_.end())
            {
                ed_to_node_.erase(it->second);
                node_to_ed_.erase(it);
            }
            for (auto it = pin_to_ed_.begin(); it != pin_to_ed_.end();)
            {
                if (it->first.node == node)
                {
                    ed_to_pin_.erase(it->second);
                    it = pin_to_ed_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            for (auto it = link_to_ed_.begin(); it != link_to_ed_.end();)
            {
                if (it->first.from.node == node || it->first.to.node == node)
                {
                    ed_to_link_.erase(it->second);
                    it = link_to_ed_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        void clear()
        {
            node_to_ed_.clear();
            ed_to_node_.clear();
            pin_to_ed_.clear();
            ed_to_pin_.clear();
            link_to_ed_.clear();
            ed_to_link_.clear();
            next_ = 1;
        }

    private:
        struct PinKey
        {
            node_id       node = kInvalidNode;
            std::uint32_t pin  = kInvalidPin;
            std::uint8_t  side = 0;

            bool operator==(const PinKey&) const = default;
        };

        struct LinkKey
        {
            PinKey from;
            PinKey to;

            bool operator==(const LinkKey&) const = default;
        };

        static PinKey pinKey(GraphPinRef pin)
        {
            return PinKey{ pin.node.id, pin.pin, static_cast<std::uint8_t>(pin.side) };
        }

        static std::size_t hashCombine(std::size_t seed, std::size_t v)
        {
            return seed ^ (v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
        }

        struct PinKeyHash
        {
            std::size_t operator()(const PinKey& k) const noexcept
            {
                std::size_t h = std::hash<node_id>{}(k.node);
                h = hashCombine(h, k.pin);
                return hashCombine(h, k.side);
            }
        };

        struct LinkKeyHash
        {
            std::size_t operator()(const LinkKey& k) const noexcept
            {
                return hashCombine(PinKeyHash{}(k.from), PinKeyHash{}(k.to));
            }
        };

        std::uintptr_t next_ = 1;

        std::unordered_map<node_id, std::uintptr_t>           node_to_ed_;
        std::unordered_map<std::uintptr_t, node_id>           ed_to_node_;
        std::unordered_map<PinKey, std::uintptr_t, PinKeyHash> pin_to_ed_;
        std::unordered_map<std::uintptr_t, GraphPinRef>       ed_to_pin_;
        std::unordered_map<LinkKey, std::uintptr_t, LinkKeyHash> link_to_ed_;
        std::unordered_map<std::uintptr_t, std::pair<GraphPinRef, GraphPinRef>> ed_to_link_;
    };

} // namespace lux::graphkit::detail
