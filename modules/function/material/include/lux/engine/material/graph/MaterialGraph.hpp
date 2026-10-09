#pragma once
// =============================================================================
//  MaterialGraph.hpp  —  Material graph container (pure data model)
// -----------------------------------------------------------------------------
//  Authoring source document. Runtime consumes only its cooked MaterialData.
// =============================================================================

#include <memory>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include <lux/engine/description/MaterialEnums.hpp>
#include <lux/engine/function/graph/GraphEdit.hpp>
#include <lux/engine/material/graph/MaterialNode.hpp>
#include <lux/engine/material/graph/visibility.h>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <variant>

namespace lux::material
{
    using graph::NodeId;
    using graph::PinId;

    using VMaterialGraphFailure = std::variant<graph::GraphTopologyFailure, MaterialCompileFailure>;
    template <class T> using MaterialGraphResult = cxx::expected<T, VMaterialGraphFailure>;

    class MaterialGraphEdit;

    namespace detail
    {
        struct MaterialSourceAccess;
    }

    /// A texture slot declared by the graph (-> ShadingModelDescriptor + descriptor
    /// layout set 2).
    struct TextureSlotDecl
    {
        std::string name;
        lux::asset::AssetId texture;
        friend bool operator==(const TextureSlotDecl&, const TextureSlotDecl&) = default;
    };

    /// A scalar/vector parameter declared by the graph (-> material SSBO set 4).
    struct ParamSlotDecl
    {
        std::string name;
        EValueType type = EValueType::FLOAT;
        float dflt[4] = {0, 0, 0, 0};
        friend bool operator==(const ParamSlotDecl&, const ParamSlotDecl&) = default;
    };

    /// Render state (pipeline-related, not a surface attribute): alpha blend mode,
    /// cutout threshold, double-sided. Reuses the existing EAlphaMode (the same
    /// enum used by built-in materials). Mask is realized by emitting `discard` in
    /// the fragment shader at bake time; Blend/double-sided are PSO state (part of
    /// the bucket key).
    struct RenderState
    {
        lux::rdesc::EAlphaMode alpha_mode = lux::rdesc::EAlphaMode::OPAQUE_SURFACE;
        float alpha_cutoff = 0.5f;
        bool double_sided = false;
    };

    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialGraph final
    {
    public:
        MaterialGraph() noexcept;
        ~MaterialGraph();
        MaterialGraph(const MaterialGraph&) = delete;
        MaterialGraph& operator=(const MaterialGraph&) = delete;
        MaterialGraph(MaterialGraph&&) noexcept;
        MaterialGraph& operator=(MaterialGraph&&) noexcept;

        [[nodiscard]] MaterialGraphResult<MaterialGraph> clone() const noexcept;
        [[nodiscard]] MaterialGraphResult<NodeId> addNode(MaterialNode) noexcept;
        [[nodiscard]] MaterialGraphResult<NodeId> addNodeWithId(
            NodeId,
            MaterialNode,
            std::span<const MaterialPinEntry> = {}
        ) noexcept;
        [[nodiscard]] MaterialGraphResult<MaterialNodeSnapshot> extractNode(NodeId) noexcept;
        [[nodiscard]] MaterialGraphResult<void> removeNode(NodeId) noexcept;

        [[nodiscard]] const MaterialNode* node(NodeId) const noexcept;
        [[nodiscard]] const MaterialPinPayload* pin(PinId) const noexcept;
        // Metadata edits do not change structural records. Source/compile validation still checks them.
        [[nodiscard]] MaterialPinPayload* pin(PinId) noexcept;
        [[nodiscard]] PinId pinId(NodeId, graph::PinSemanticId) const noexcept;
        [[nodiscard]] MaterialGraphResult<void> connect(PinId from, PinId to) noexcept;
        [[nodiscard]] MaterialGraphResult<void> disconnect(PinId input) noexcept;

        [[nodiscard]] const graph::GraphTopology& topology() const noexcept;
        [[nodiscard]] const graph::GraphLayout& layout() const noexcept;

        [[nodiscard]] auto nodes() const noexcept
        {
            return std::views::transform(
                nodes_,
                [](const auto& entry) noexcept
                { return std::pair<NodeId, const MaterialNode*>{entry.first, &entry.second}; }
            );
        }

        rdesc::ELightingTechnique shading_model{rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS};
        std::vector<TextureSlotDecl> texture_slots;
        std::vector<ParamSlotDecl> param_slots;
        RenderState render_state;

    private:
        friend class MaterialGraphEdit;
        friend struct detail::MaterialSourceAccess;
        using NodeStorage = std::unordered_map<NodeId, MaterialNode>;
        using PinStorage = std::unordered_map<PinId, MaterialPinPayload>;

        NodeStorage nodes_;
        PinStorage pins_;
        graph::GraphTopology topology_;
        graph::GraphLayout layout_;
    };

    // Invalid id requests fresh node/pin identities. Restored pins must belong to the explicit id.
    // Empty pins use the registered schema; provided records must match every schema semantic.
    struct MaterialNodeEntry final
    {
        NodeId id;
        const MaterialNode* value{};
        std::span<const MaterialPinEntry> pins;
    };

    struct MaterialGraphChange final
    {
        std::span<const MaterialNodeEntry> insert;
        std::span<const NodeId> erase;
        std::span<const graph::LinkRecord> connect;
        std::span<const graph::LinkRecord> disconnect;
        std::span<const graph::GraphLayoutEntry> place;
        std::span<const NodeId> unplace;
    };

    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialGraphEdit final
    {
    public:
        [[nodiscard]] static MaterialGraphResult<MaterialGraphEdit>
        prepare(MaterialGraph&, const MaterialGraphChange&) noexcept;
        ~MaterialGraphEdit();
        MaterialGraphEdit(MaterialGraphEdit&&) noexcept;
        MaterialGraphEdit(const MaterialGraphEdit&) = delete;
        MaterialGraphEdit& operator=(const MaterialGraphEdit&) = delete;
        MaterialGraphEdit& operator=(MaterialGraphEdit&&) = delete;

        [[nodiscard]] std::span<const MaterialNodeEntry> insertedNodes() const noexcept;
        [[nodiscard]] MaterialGraphResult<void> place(NodeId, graph::GraphNodeLayout) noexcept;
        // One exclusive synchronous borrow until commit/destruction. Commit transfers prepared node
        // handles and swaps the original GraphEdit candidates; removed code owners survive until cleanup.
        void commit() noexcept;

    private:
        friend class MaterialGraph;
        using NodeStorage = MaterialGraph::NodeStorage;
        using PinStorage = MaterialGraph::PinStorage;
        explicit MaterialGraphEdit(MaterialGraph&) noexcept;
        [[nodiscard]] MaterialGraphResult<void>
        insert(NodeId, MaterialNode, std::span<const MaterialPinEntry>) noexcept;
        void reserveCommit() noexcept;

        MaterialGraph* target_;
        graph::GraphEdit structure_;
        NodeStorage staged_nodes_;
        PinStorage staged_pins_;
        std::vector<NodeStorage::node_type> retired_nodes_;
        std::vector<PinStorage::node_type> retired_pins_;
        std::vector<NodeId> erase_nodes_;
        std::vector<PinId> erase_pins_;
        std::vector<std::vector<MaterialPinEntry>> inserted_pins_;
        std::vector<MaterialNodeEntry> inserted_;
        bool committed_{};
    };
} // namespace lux::material
