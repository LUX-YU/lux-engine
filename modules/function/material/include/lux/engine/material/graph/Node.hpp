#pragma once
// =============================================================================
//  Node.hpp  —  Material graph node base class
// -----------------------------------------------------------------------------
//  A polymorphic node plus its pin container (mirrors FlowForge's Node, minus
//  exec/token/ENodeOperation). Concrete nodes are in Nodes.hpp.
// =============================================================================

#include <memory>
#include <string>
#include <vector>
#include <utility>

#include <lux/engine/material/graph/DataPin.hpp>
#include <lux/engine/material/graph/visibility.h>

namespace lux::material
{
    /**
     * @brief Kinds of material graph nodes. Append new node kinds before COUNT.
     */
    enum class EMatNodeKind : uint16_t
    {
        Invalid = 0,
        Constant,       ///< 0 in, 1 out — a constant value
        Input,          ///< 0 in, 1 out — a single MaterialInput (uv0 / world_normal / ...)
        SampleTexture,  ///< 1 in (uv), 1 out (vec4) — samples a bindless texture (set 2)
        Math,           ///< 2 in, 1 out — arithmetic (op selected via payload)
        Swizzle,        ///< 1 in, 1 out
        Construct,      ///< n in, 1 out — assembles a vector
        DecodeNormal,   ///< 1 in (rgb), 1 out (vec3) — normal-map decode (tangent space)
        TbnTransform,   ///< 1 in (tangent-space normal), 1 out — TBN transform to world space
        Param,          ///< 0 in, 1 out — reads a per-material parameter slot (set 4, Graph family)
        OutputSurface,  ///< n in (one per surface attribute), 0 out — terminal node
        COUNT
    };

    LUX_ENGINE_MATERIAL_GRAPH_PUBLIC const char* toString(
        EMatNodeKind kind) noexcept;

    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC Node
    {
    public:
        explicit Node(EMatNodeKind kind) : kind_(kind) {}
        virtual ~Node();  // Defined in Node.cpp (MSVC export anchor)

        Node& operator=(const Node&) = delete;

        /// Deep copy (polymorphic clone): each subclass returns a copy of the same
        /// type, with the same payload and the same pins (including connections /
        /// constants). The editor uses this to produce an editable working copy
        /// from the graph an asset owns (the graph itself is move-only).
        [[nodiscard]] virtual std::unique_ptr<Node> clone() const = 0;

        node_id      id() const noexcept { return id_; }
        void         setId(node_id id) noexcept { id_ = id; }
        EMatNodeKind kind() const noexcept { return kind_; }

        /// Safe downcast to a concrete node type — no RTTI. Keys kind() against
        /// the target's `kKind` constant (every concrete node declares one).
        /// Returns nullptr on a kind mismatch, exactly like the dynamic_cast it
        /// replaces.
        template<class T> [[nodiscard]] T* as() noexcept
        { return kind_ == T::kKind ? static_cast<T*>(this) : nullptr; }

        template<class T> [[nodiscard]] const T* as() const noexcept
        { return kind_ == T::kKind ? static_cast<const T*>(this) : nullptr; }

        const std::string& name() const noexcept { return name_; }
        void               setName(std::string n) { name_ = std::move(n); }

        std::vector<DataPin>&       inputs()  noexcept { return in_pins_; }
        const std::vector<DataPin>& inputs()  const noexcept { return in_pins_; }
        std::vector<DataPin>&       outputs() noexcept { return out_pins_; }
        const std::vector<DataPin>& outputs() const noexcept { return out_pins_; }

        // ---- Editor metadata (not read by lowering; persisted with the graph since LMGR v2) ----
        // Canvas position is read/written by GraphKit via IGraphView::nodePos/setNodePos.
        // After decoding a v1 asset, ui_placed=false, so GraphLayout arranges nodes on a
        // grid; the next save then upgrades it to v2. clone() carries this over
        // automatically via the protected copy constructor.
        float ui_pos[2] = { 0.0f, 0.0f };
        bool  ui_placed = false;

    protected:
        // Protected copy constructor: only for subclass clone() to use
        // (`make_unique<Derived>(*this)`); still non-copyable from the outside
        // (prevents slicing).
        Node(const Node&) = default;

        node_id              id_ = invalid_node;
        EMatNodeKind         kind_;
        std::string          name_;
        std::vector<DataPin> in_pins_;
        std::vector<DataPin> out_pins_;
    };

} // namespace lux::material
