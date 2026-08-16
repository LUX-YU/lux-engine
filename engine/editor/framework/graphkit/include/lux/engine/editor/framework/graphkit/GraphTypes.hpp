#pragma once
// =============================================================================
//  GraphTypes.hpp — GraphKit's shared vocabulary (pure aggregates, no imgui).
//
//  GraphKit is the ONE shared node-graph editor framework. It never owns graph
//  data: every type here is either a REFERENCE into a borrowed domain SSOT
//  (GraphNodeRef / GraphPinRef — always ids + indices, NEVER pointers, so they
//  survive a domain-side pin rebuild) or a per-frame SNAPSHOT the editor reads
//  through the IGraphView port and immediately consumes (GraphPinView /
//  GraphLinkView — never stored).
//
//  Pin TYPES are split by layer (the A/B decision): the authoritative typed
//  form (rdesc::EMatValueType / lux::meta::RefType) stays per-domain and never
//  crosses up; GraphPinType here is an OPAQUE, derived, draw-only token the
//  schema styles and the editor validates caps with.
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <lux/engine/editor/framework/graphkit/visibility_graphkit.h>

namespace lux::graphkit
{
    using node_id = std::uint64_t;

    inline constexpr node_id       kInvalidNode = ~0ull;
    inline constexpr std::uint32_t kInvalidPin  = ~0u;

    /// Unlimited fan cap sentinel (see GraphPinType::fan_cap).
    inline constexpr std::uint8_t kFanUnlimited = 0xFF;

    enum class EPinSide : std::uint8_t
    {
        INPUT,
        OUTPUT
    };

    /// Tiny 2D vector for canvas positions (GraphKit does not pull a math lib).
    struct GraphVec2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    /// Reference to a domain node — the domain's stable node id.
    struct GraphNodeRef
    {
        node_id id = kInvalidNode;

        bool valid() const noexcept { return id != kInvalidNode; }
        bool operator==(const GraphNodeRef&) const = default;
    };

    /// Reference to a domain pin: (node id, side, pin index). NEVER a raw
    /// domain Pin* — flowforge rebuilds (deletes) its pin objects on
    /// rebind/reconstruct; indices stay valid until reconstructNode.
    struct GraphPinRef
    {
        GraphNodeRef  node;
        EPinSide      side = EPinSide::INPUT;
        std::uint32_t pin  = kInvalidPin;

        bool valid() const noexcept { return node.valid() && pin != kInvalidPin; }
        bool operator==(const GraphPinRef&) const = default;
    };

    /// OPAQUE draw-only pin-type token, derived per frame and never stored or
    /// persisted. `category` is domain-tagged (the adapter packs a domain id
    /// into the high byte) so two domains' categories can never alias in a
    /// styling table. `fan_cap` is the max simultaneous link count on THIS
    /// pin's side (direction-relative): material data-in = 1, data-out =
    /// unlimited; flowforge exec-in = unlimited (fan-in join), exec-out = 1,
    /// data-in = 1, data-out = unlimited. The editor uses caps to drive
    /// replace-on-reconnect for cap-1 pins.
    struct GraphPinType
    {
        std::uint32_t category    = 0;
        EPinSide      direction   = EPinSide::INPUT;
        std::uint8_t  fan_cap     = 1;
        bool          is_wildcard = false; ///< PC_Wildcard analogue (reserved; no domain emits it yet)
    };

    /// Per-frame pin snapshot (the read core yields display data — both
    /// shipped panels draw pin labels). `name` borrows from the domain and is
    /// only valid during the enclosing port call.
    struct GraphPinView
    {
        std::string_view name;
        GraphPinType     type;
        bool             has_link = false;
    };

    /// Per-frame link snapshot: from = an OUTPUT pin, to = an INPUT pin.
    struct GraphLinkView
    {
        GraphPinRef from;
        GraphPinRef to;
    };

    /// Palette entry. String identity mirrors flowforge's NodeCreatInfo
    /// {name, category, creator}: open registries forward their records 1:1,
    /// closed-enum domains expose a fixed table. The factory stays DOMAIN-side
    /// behind IGraphView::addNode(template_id).
    struct NodeTemplate
    {
        std::string id;           ///< stable identity handed back to addNode()
        std::string display_name;
        std::string category;     ///< palette grouping
    };

    /// Pin styling descriptor (the AssetTypeDesc table idiom: adapters are
    /// advised to back pinStyle() with a static enum-ordinal table).
    /// `color` is IM_COL32-packed (ABGR), kept as uint32 so this header stays
    /// imgui-free.
    struct PinStyleDesc
    {
        std::uint32_t color = 0xFFFFFFFFu;
    };

    /// Node chrome descriptor (blueprint-style colored title band; lifted from
    /// MaterialGraphPanel's MatNodeBuilder so every host shares one look).
    /// `header_color` is IM_COL32-packed (ABGR), imgui-free like PinStyleDesc;
    /// the editor forces the band opaque and modulates by the global alpha.
    struct NodeStyleDesc
    {
        std::uint32_t header_color = 0xFF646464u; ///< neutral grey default
        bool          draw_header  = true;        ///< false => plain title text
    };

    /// A schema-provided per-node action (e.g. SEQUENCE "Add exec pin"),
    /// surfaced by the editor in the node context menu.
    struct NodeAction
    {
        std::string id;
        std::string label;
    };

    /// canConnect verdict; `reason` feeds the drag-reject tooltip.
    struct ConnectResult
    {
        bool        allowed = false;
        std::string reason;

        static ConnectResult yes() { return ConnectResult{ true, {} }; }
        static ConnectResult no(std::string why) { return ConnectResult{ false, std::move(why) }; }
    };

} // namespace lux::graphkit
