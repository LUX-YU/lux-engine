#pragma once
// =============================================================================
//  MaterialGraph.hpp  —  Material graph container (pure data model)
// -----------------------------------------------------------------------------
//  Authoring source document. Runtime consumes only its cooked MaterialData.
// =============================================================================

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <lux/engine/material/graph/Node.hpp>
#include <lux/engine/function/graph/GraphLayout.hpp>
#include <lux/engine/description/MaterialEnums.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/material/graph/visibility.h>

namespace lux::material
{
    /// A texture slot declared by the graph (-> ShadingModelDescriptor + descriptor
    /// layout set 2).
    struct TextureSlotDecl
    {
        std::string name;
        lux::asset::AssetId texture;
    };

    /// A scalar/vector parameter declared by the graph (-> material SSBO set 4).
    struct ParamSlotDecl
    {
        std::string   name;
        EValueType type    = EValueType::FLOAT;
        float         dflt[4] = { 0, 0, 0, 0 };
    };

    /// Render state (pipeline-related, not a surface attribute): alpha blend mode,
    /// cutout threshold, double-sided. Reuses the existing EAlphaMode (the same
    /// enum used by built-in materials). Mask is realized by emitting `discard` in
    /// the fragment shader at bake time; Blend/double-sided are PSO state (part of
    /// the bucket key).
    struct RenderState
    {
        lux::rdesc::EAlphaMode alpha_mode = lux::rdesc::EAlphaMode::Opaque;
        float      alpha_cutoff = 0.5f;
        bool       double_sided = false;
    };

    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialGraph
    {
    public:
        MaterialGraph();
        ~MaterialGraph();
        MaterialGraph(const MaterialGraph&) = delete;
        MaterialGraph& operator=(const MaterialGraph&) = delete;
        MaterialGraph(MaterialGraph&&) noexcept;
        MaterialGraph& operator=(MaterialGraph&&) noexcept;

        /// Deep copy (the graph is move-only: nodes are polymorphic unique_ptr
        /// objects). Preserves node ids, all connections/constants, slot
        /// declarations, and render state. The editor uses this to produce a
        /// working copy from the graph an asset owns.
        [[nodiscard]] MaterialGraph clone() const;

        NodeId     addNode(std::unique_ptr<Node> node);
        Node*       node(NodeId id) noexcept;
        const Node* node(NodeId id) const noexcept;
        void        removeNode(NodeId id);

        /// Inserts a node with a specific id (GraphKit's undo relies on stable ids:
        /// restoring a deleted node must reuse its original id, since both
        /// connections and recorded undo actions reference nodes by id). Returns
        /// Invalid NodeId if the id is already taken, invalid, or node is null;
        /// next_id_ is bumped to the high-water mark so later addNode calls never
        /// collide with it.
        NodeId addNodeWithId(NodeId id, std::unique_ptr<Node> node);

        /// Removes a node without destroying it and without touching other nodes'
        /// input connections (the editor disconnects the recorded links one by one
        /// before extracting). Returns nullptr if the node doesn't exist.
        [[nodiscard]] std::unique_ptr<Node> extractNode(NodeId id);

        /// Connects output pin src_pin of src to input pin dst_pin of dst. Returns
        /// whether the connection succeeded.
        [[nodiscard]] bool canConnect(NodeId src, uint32_t src_pin, NodeId dst, uint32_t dst_pin) const noexcept;
        bool connect(NodeId src, uint32_t src_pin, NodeId dst, uint32_t dst_pin);
        void disconnect(NodeId dst, uint32_t dst_pin);
        [[nodiscard]] PinLink source(NodeId dst, uint32_t dst_pin) const noexcept;
        [[nodiscard]] PinLink source(PinId input) const noexcept;

        [[nodiscard]] lux::graph::GraphTopology& topology() noexcept { return topology_; }
        [[nodiscard]] const lux::graph::GraphTopology& topology() const noexcept { return topology_; }
        [[nodiscard]] lux::graph::GraphLayout& layout() noexcept { return layout_; }
        [[nodiscard]] const lux::graph::GraphLayout& layout() const noexcept { return layout_; }

        const std::unordered_map<NodeId, std::unique_ptr<Node>>& nodes() const noexcept
        {
            return nodes_;
        }

        lux::rdesc::ELightingTechnique shading_model = lux::rdesc::ELightingTechnique::PbrMetallicRoughness;
        std::vector<TextureSlotDecl> texture_slots;
        std::vector<ParamSlotDecl>   param_slots;
        RenderState                  render_state;

    private:
        [[nodiscard]] bool registerNodeStructure(Node& node, bool preserve_pin_ids) noexcept;

        std::unordered_map<NodeId, std::unique_ptr<Node>> nodes_;
        lux::graph::GraphTopology topology_;
        lux::graph::GraphLayout layout_;
    };

} // namespace lux::material
