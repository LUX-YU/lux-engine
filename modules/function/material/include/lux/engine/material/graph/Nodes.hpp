#pragma once
// =============================================================================
//  Nodes.hpp  —  Concrete material nodes (each carries its own payload)
// -----------------------------------------------------------------------------
//  Each node's constructor is responsible for setting up its own input/output
//  pins per the contract (see Node.cpp). Getting the basic skeleton working only
//  needs ConstantNode + OutputSurfaceNode; the rest exist as scaffolding for
//  future expansion.
// =============================================================================

#include <memory>

#include <lux/engine/material/graph/Node.hpp>

namespace lux::material
{
    enum class EMathOp : uint8_t
    {
        // binary (2 operands)
        MUL,
        ADD,
        SUB,
        DIV,
        DOT,        ///< -> Float
        MIN,
        MAX,
        POW,
        STEP,
        MOD,
        CROSS,      ///< Vec3 x Vec3 -> Vec3
        REFLECT,
        // ternary (currently unsupported by the 2-pin Math node)
        LERP,
        // unary (1 operand; pin 1 ignored)
        SATURATE,
        ONE_MINUS, ///< 1 - x
        ABS,
        SQRT,
        FLOOR,
        FRACT,
        SIN,
        COS,
        NORMALIZE,
        LENGTH ///< -> Float
    };

    /// Constant node: outputs a single constant value.
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC ConstantNode final : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::CONSTANT;
        ConstantNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<ConstantNode>(*this); }

        float         value[4]   = { 0, 0, 0, 0 };
        EValueType value_type = EValueType::VEC4;

        /// Sets the constant's type and updates the output pin to match (called
        /// by the editor when it changes the arity).
        void setType(EValueType t);
    };

    /// Input node: exposes a single shading input (uv0 / world_normal / ...).
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC InputNode final : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::INPUT;
        InputNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<InputNode>(*this); }

        EMaterialInput input = EMaterialInput::UV0;
    };

    /// Samples a bindless texture (set 2); `texture_slot` indexes the texture
    /// slots declared by the graph.
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC SampleTextureNode final : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::SAMPLE_TEXTURE;
        SampleTextureNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<SampleTextureNode>(*this); }

        uint32_t texture_slot = 0;
    };

    /// Reads a per-material parameter (a param_slot declared by the graph;
    /// supplied at runtime by the Graph-family SSBO). `param_slot` indexes
    /// MaterialGraph::param_slots; `type` is that parameter's type (used for the
    /// output pin).
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC ParamNode final : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::PARAM;
        explicit ParamNode(EValueType type = EValueType::VEC4);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<ParamNode>(*this); }

        uint32_t      param_slot = 0;
        EValueType type;

        /// Sets the parameter's type and updates the output pin to match (called
        /// by the editor when it changes the arity).
        void setType(EValueType t);
    };

    /// Arithmetic node. `operand_type` determines the type of both operand pins
    /// (Float by default); use setOperandType to express vector operations
    /// (Vec3*Vec3, Vec3·Vec3, etc.).
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MathNode final : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::MATH;
        explicit MathNode(EMathOp op = EMathOp::MUL);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<MathNode>(*this); }

        EMathOp       op;
        EValueType operand_type = EValueType::FLOAT;

        /// Sets the operand type and updates both input pins to match (and the
        /// output pin, but only as an editor hint — lowering still derives the
        /// real result type from `op`; e.g. Dot always produces Float).
        void setOperandType(EValueType t);
    };

    /// Normal-map decode (rgb -> tangent-space normal).
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC DecodeNormalNode final : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::DECODE_NORMAL;
        DecodeNormalNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<DecodeNormalNode>(*this); }
    };

    /// Transforms a tangent-space normal into world space: mat3(T,B,N) * n
    /// (using the interpolated world-space tangent/bitangent/normal). Pairs with
    /// DecodeNormal so normal maps participate correctly in lighting.
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC TbnTransformNode final : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::TBN_TRANSFORM;
        TbnTransformNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<TbnTransformNode>(*this); }
    };

    /// Component reshuffle / truncation (e.g. a vec4 texture's .rgb -> vec3, so
    /// a texture can feed a vec3 attribute). `components[k]` = which component of
    /// the source vector (0=x,1=y,2=z,3=w) feeds output channel k; only the first
    /// arity(out_type) entries are used. Defaults to vec4 -> vec3 via .xyz.
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC SwizzleNode final : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::SWIZZLE;
        explicit SwizzleNode(EValueType source_type = EValueType::VEC4,
                             EValueType out_type    = EValueType::VEC3);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<SwizzleNode>(*this); }

        EValueType source_type;
        EValueType out_type;
        uint8_t       components[4] = { 0, 1, 2, 3 };

        /// Sets the source/output types and updates the pins to match (the input
        /// pin's type is authoritative — resolveInput type-checks against it).
        void setTypes(EValueType source_type, EValueType out_type);
    };

    /// Construct: packs arity(out_type) scalar components into a vector
    /// (vec3(x,y,z), etc.). All input pins are Float, and their count equals the
    /// output vector's component count. The dual of Swizzle.
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC ConstructNode final : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::CONSTRUCT;
        explicit ConstructNode(EValueType out_type = EValueType::VEC3);
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<ConstructNode>(*this); }

        EValueType out_type;
    };

    /// Terminal node: one input pin per EMaterialAttribute (in contract order).
    class LUX_ENGINE_MATERIAL_GRAPH_PUBLIC OutputSurfaceNode final : public Node
    {
    public:
        static constexpr EMatNodeKind kKind = EMatNodeKind::OUTPUT_SURFACE;
        OutputSurfaceNode();
        [[nodiscard]] std::unique_ptr<Node> clone() const override { return std::make_unique<OutputSurfaceNode>(*this); }
    };

} // namespace lux::material
