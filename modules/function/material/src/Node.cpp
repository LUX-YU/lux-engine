// =============================================================================
//  Node.cpp  —  Node's export anchor + construction of concrete nodes (sets up
//  pins per the contract)
// =============================================================================

#include <lux/engine/material/graph/Node.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

namespace lux::material
{
    Node::~Node() = default;

    const char* toString(EMatNodeKind kind) noexcept
    {
        switch (kind)
        {
        case EMatNodeKind::CONSTANT:      return "Constant";
        case EMatNodeKind::INPUT:         return "Input";
        case EMatNodeKind::SAMPLE_TEXTURE: return "Sample Texture";
        case EMatNodeKind::MATH:          return "Math";
        case EMatNodeKind::SWIZZLE:       return "Swizzle";
        case EMatNodeKind::CONSTRUCT:     return "Construct";
        case EMatNodeKind::DECODE_NORMAL:  return "Decode Normal";
        case EMatNodeKind::TBN_TRANSFORM:  return "TBN Transform";
        case EMatNodeKind::PARAM:         return "Param";
        case EMatNodeKind::OUTPUT_SURFACE: return "Output Surface";
        default:                          return "Invalid";
        }
    }

    namespace
    {
        DataPin makePin(const char* name, EValueType type, EPinDirection dir)
        {
            DataPin p{};
            p.name      = name;
            p.type      = type;
            p.direction = dir;
            return p;
        }
    } // namespace

    // ---- ConstantNode -------------------------------------------------------
    ConstantNode::ConstantNode() : Node(ConstructionKey{}, EMatNodeKind::CONSTANT)
    {
        setName("Constant");
        out_pins_.push_back(makePin("out", value_type, EPinDirection::OUTPUT));
    }

    void ConstantNode::setType(EValueType t)
    {
        value_type = t;
        if (!out_pins_.empty())
            out_pins_[0].type = t;
    }

    // ---- InputNode ----------------------------------------------------------
    InputNode::InputNode() : Node(ConstructionKey{}, EMatNodeKind::INPUT)
    {
        setName("Input");
        out_pins_.push_back(makePin("out", EValueType::VEC2, EPinDirection::OUTPUT));
    }

    // ---- SampleTextureNode --------------------------------------------------
    SampleTextureNode::SampleTextureNode() : Node(ConstructionKey{}, EMatNodeKind::SAMPLE_TEXTURE)
    {
        setName("Sample Texture");
        in_pins_.push_back(makePin("uv", EValueType::VEC2, EPinDirection::INPUT));
        out_pins_.push_back(makePin("rgba", EValueType::VEC4, EPinDirection::OUTPUT));
    }

    // ---- MathNode -----------------------------------------------------------
    MathNode::MathNode(EMathOp o) : Node(ConstructionKey{}, EMatNodeKind::MATH), op(o)
    {
        setName("Math");
        in_pins_.push_back(makePin("a", EValueType::FLOAT, EPinDirection::INPUT));
        in_pins_.push_back(makePin("b", EValueType::FLOAT, EPinDirection::INPUT));
        out_pins_.push_back(makePin("result", EValueType::FLOAT, EPinDirection::OUTPUT));
    }

    void MathNode::setOperandType(EValueType t)
    {
        operand_type = t;
        if (in_pins_.size() >= 2)
        {
            in_pins_[0].type = t;  // Input pin type is authoritative — resolveInput type-checks against it
            in_pins_[1].type = t;
        }
        if (!out_pins_.empty())
            out_pins_[0].type = t;  // Hint only; lowering derives Float for ops like Dot regardless
    }

    // ---- DecodeNormalNode ---------------------------------------------------
    DecodeNormalNode::DecodeNormalNode() : Node(ConstructionKey{}, EMatNodeKind::DECODE_NORMAL)
    {
        setName("Decode Normal");
        in_pins_.push_back(makePin("rgb", EValueType::VEC3, EPinDirection::INPUT));
        out_pins_.push_back(makePin("normal", EValueType::VEC3, EPinDirection::OUTPUT));
    }

    // ---- SwizzleNode --------------------------------------------------------
    SwizzleNode::SwizzleNode(EValueType source, EValueType out)
        : Node(ConstructionKey{}, EMatNodeKind::SWIZZLE), source_type(source), out_type(out)
    {
        setName("Swizzle");
        in_pins_.push_back(makePin("in", source, EPinDirection::INPUT));
        out_pins_.push_back(makePin("out", out, EPinDirection::OUTPUT));
    }

    void SwizzleNode::setTypes(EValueType source, EValueType out)
    {
        source_type = source;
        out_type    = out;
        if (!in_pins_.empty())  in_pins_[0].type  = source;  // Input pin type is authoritative
        if (!out_pins_.empty()) out_pins_[0].type = out;
    }

    // ---- ParamNode ----------------------------------------------------------
    ParamNode::ParamNode(EValueType t) : Node(ConstructionKey{}, EMatNodeKind::PARAM), type(t)
    {
        setName("Param");
        out_pins_.push_back(makePin("value", t, EPinDirection::OUTPUT));
    }

    void ParamNode::setType(EValueType t)
    {
        type = t;
        if (!out_pins_.empty())
            out_pins_[0].type = t;  // Output pin type is authoritative
    }

    // ---- TbnTransformNode ---------------------------------------------------
    TbnTransformNode::TbnTransformNode() : Node(ConstructionKey{}, EMatNodeKind::TBN_TRANSFORM)
    {
        setName("TBN Transform");
        in_pins_.push_back(makePin("normal_ts", EValueType::VEC3, EPinDirection::INPUT));
        out_pins_.push_back(makePin("world_normal", EValueType::VEC3, EPinDirection::OUTPUT));
    }

    // ---- ConstructNode ------------------------------------------------------
    ConstructNode::ConstructNode(EValueType out) : Node(ConstructionKey{}, EMatNodeKind::CONSTRUCT), out_type(out)
    {
        setName("Construct");
        const int n = (out == EValueType::FLOAT) ? 1
                    : (out == EValueType::VEC2)  ? 2
                    : (out == EValueType::VEC3)  ? 3
                                                    : 4;
        static const char* const kComp[4] = { "x", "y", "z", "w" };
        for (int i = 0; i < n; ++i)
            in_pins_.push_back(makePin(kComp[i], EValueType::FLOAT, EPinDirection::INPUT));
        out_pins_.push_back(makePin("out", out, EPinDirection::OUTPUT));
    }

    // ---- OutputSurfaceNode --------------------------------------------------
    // One input pin per EMaterialAttribute; name/type/default come from the
    // contract table, and pin index i corresponds directly to attribute i
    // (so lowering can map between them with no extra lookup).
    OutputSurfaceNode::OutputSurfaceNode() : Node(ConstructionKey{}, EMatNodeKind::OUTPUT_SURFACE)
    {
        setName("Output Surface");
        for (const auto& a : kMaterialAttributes)
        {
            DataPin pin = makePin(a.name, a.type, EPinDirection::INPUT);
            pin.constant[0] = a.dflt[0];
            pin.constant[1] = a.dflt[1];
            pin.constant[2] = a.dflt[2];
            pin.constant[3] = a.dflt[3];
            in_pins_.push_back(std::move(pin));
        }
    }

} // namespace lux::material
