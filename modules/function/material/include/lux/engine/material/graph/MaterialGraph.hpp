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

        node_id     addNode(std::unique_ptr<Node> node);
        Node*       node(node_id id) noexcept;
        const Node* node(node_id id) const noexcept;
        void        removeNode(node_id id);

        /// Inserts a node with a specific id (GraphKit's undo relies on stable ids:
        /// restoring a deleted node must reuse its original id, since both
        /// connections and recorded undo actions reference nodes by id). Returns
        /// invalid_node if the id is already taken, invalid, or node is null;
        /// next_id_ is bumped to the high-water mark so later addNode calls never
        /// collide with it.
        node_id addNodeWithId(node_id id, std::unique_ptr<Node> node);

        /// Removes a node without destroying it and without touching other nodes'
        /// input connections (the editor disconnects the recorded links one by one
        /// before extracting). Returns nullptr if the node doesn't exist.
        [[nodiscard]] std::unique_ptr<Node> extractNode(node_id id);

        /// Connects output pin src_pin of src to input pin dst_pin of dst. Returns
        /// whether the connection succeeded.
        bool connect(node_id src, uint32_t src_pin, node_id dst, uint32_t dst_pin);
        void disconnect(node_id dst, uint32_t dst_pin);

        const std::unordered_map<node_id, std::unique_ptr<Node>>& nodes() const noexcept
        {
            return nodes_;
        }

        lux::rdesc::ELightingTechnique shading_model = lux::rdesc::ELightingTechnique::PbrMetallicRoughness;
        std::vector<TextureSlotDecl> texture_slots;
        std::vector<ParamSlotDecl>   param_slots;
        RenderState                  render_state;

    private:
        std::unordered_map<node_id, std::unique_ptr<Node>> nodes_;
        node_id                                            next_id_ = 1;
    };

} // namespace lux::material
