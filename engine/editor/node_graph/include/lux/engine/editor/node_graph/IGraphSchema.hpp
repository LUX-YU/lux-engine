#pragma once
// =============================================================================
//  IGraphSchema.hpp — per-domain editor semantics (the UEdGraphSchema
//  analogue): connection rules, palette, pin styling, wildcard hooks, node
//  actions, and the two day-1 parity hooks the shipped panels prove necessary:
//
//   * drawNodeBody — inline widgets inside the node body (generalizes the
//     domain graph NodePainter virtual: color swatch, texture-slot button, type
//     combo, constant field). Implementations draw with plain ImGui.
//
//   * the DEFERRED POPUP queue — anything ImGui-popup-shaped (Combo dropdown,
//     color picker, search popup) MISBEHAVES inside imgui-node-editor's
//     transformed canvas space (the documented MaterialGraphPanel gotcha).
//     drawNodeBody therefore only ENQUEUES a typed request; GraphEditor
//     surfaces the queue after the canvas ends, and the HOST panel draws the
//     floating editor in panel space (exactly how MaterialGraphPanel's
//     drawSavePopup / floating color picker already work).
// =============================================================================

#include <span>
#include <string>
#include <vector>

#include "GraphTypes.hpp"
#include "IGraphView.hpp"

namespace lux::editor::node_graph
{
    /// A popup-shaped request raised from inside the node canvas, to be drawn
    /// by the HOST PANEL in normal panel space after the canvas ends.
    /// `kind` is domain-defined (e.g. "texture_picker", "color_picker");
    /// `arg` carries a small payload such as a slot index.
    struct DeferredPopupRequest
    {
        GraphNodeRef  node;
        std::string   kind;
        std::uint64_t arg = 0;
    };

    /// Collects DeferredPopupRequests during the canvas pass; the host drains
    /// it via GraphEditor::takeDeferredPopups() once per frame.
    class DeferredPopupQueue
    {
    public:
        void push(DeferredPopupRequest request) { items_.push_back(std::move(request)); }

        [[nodiscard]] std::vector<DeferredPopupRequest> take()
        {
            std::vector<DeferredPopupRequest> out;
            out.swap(items_);
            return out;
        }

        bool empty() const noexcept { return items_.empty(); }

    private:
        std::vector<DeferredPopupRequest> items_;
    };

    class IGraphSchema
    {
    public:
        virtual ~IGraphSchema() = default;

        /// Connection legality, given the WHOLE view (enables state-dependent
        /// checks: cycle rejection, fan rules, type compatibility). Material
        /// centralizes the rules that were implicit in lowerToIR; domain graph
        /// DELEGATES to the existing Pin::canLink (its connection-rule SSOT).
        /// `from` is an OUTPUT pin, `to` an INPUT pin.
        virtual ConnectResult canConnect(GraphPinRef from, GraphPinRef to,
                                         const IGraphView& view) const = 0;

        /// The palette catalog (category-grouped popup with fuzzy search).
        /// Open registries (domain graph NodeRegistry) forward their records 1:1;
        /// closed-enum domains (material EMatNodeKind) expose a fixed table.
        virtual std::span<const NodeTemplate> palette() const = 0;

        /// Styling for an opaque pin-type token (advised: back this with a
        /// static enum-ordinal table, the AssetTypeRegistry idiom).
        virtual PinStyleDesc pinStyle(const GraphPinType& type) const = 0;

        /// Node chrome (colored header band). Default = neutral grey band;
        /// return `draw_header=false` for plain text titles. Like pinStyle,
        /// best backed by a static per-kind table.
        virtual NodeStyleDesc nodeStyle(GraphNodeRef /*node*/,
                                        const IGraphView& /*view*/) const
        {
            return {};
        }

        /// Post-connect hook (wildcard resolution: copy the connected pin's
        /// type onto the wildcard node -> setType -> view.reconstructNode).
        /// Default: nothing — material emits no wildcards today.
        virtual void onPinConnected(GraphPinRef /*from*/, GraphPinRef /*to*/,
                                    IGraphView& /*view*/) {}

        /// Post-disconnect hook (wildcard DE-resolution when the last link to
        /// a resolved wildcard pin goes away). Default: nothing.
        virtual void onPinDisconnected(GraphPinRef /*from*/, GraphPinRef /*to*/,
                                       IGraphView& /*view*/) {}

        /// Per-node actions for the node context menu (e.g. SEQUENCE
        /// "Add exec pin" -> runNodeAction drives addExecOutPin +
        /// view.reconstructNode). Default: none.
        virtual std::span<const NodeAction> nodeActions(GraphNodeRef /*node*/,
                                                        const IGraphView& /*view*/) const
        {
            return {};
        }

        virtual void runNodeAction(GraphNodeRef /*node*/, std::string_view /*action_id*/,
                                   IGraphView& /*view*/) {}

        /// Inline node-body widgets (drawn INSIDE the canvas, between the pin
        /// rows). Edits made here are OUT of undo scope in v2 day-1 (today's
        /// panels have zero undo, so parity does not demand it) — recorded as
        /// a future additive command kind. Popup-shaped UI must go through
        /// `popups` (see the header comment). Default: nothing.
        virtual void drawNodeBody(GraphNodeRef /*node*/, IGraphView& /*view*/,
                                  DeferredPopupQueue& /*popups*/) {}
    };

} // namespace lux::editor::node_graph
