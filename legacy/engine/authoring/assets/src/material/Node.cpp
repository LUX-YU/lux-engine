// =============================================================================
//  Node.cpp  —  Node's export anchor + construction of concrete nodes (sets up
//  pins per the contract)
// =============================================================================

#include <lux/engine/authoring/assets/material/Node.hpp>
#include <lux/engine/authoring/assets/material/Nodes.hpp>

namespace lux::rdesc
{
    Node::~Node() = default;

    const char* toString(EMatNodeKind kind) noexcept
    {
        switch (kind)
        {
        case EMatNodeKind::Constant:      return "Constant";
        case EMatNodeKind::Input:         return "Input";
        case EMatNodeKind::SampleTexture: return "Sample Texture";
        case EMatNodeKind::Math:          return "Math";
        case EMatNodeKind::Swizzle:       return "Swizzle";
        case EMatNodeKind::Construct:     return "Construct";
        case EMatNodeKind::DecodeNormal:  return "Decode Normal";
        case EMatNodeKind::TbnTransform:  return "TBN Transform";
        case EMatNodeKind::Param:         return "Param";
        case EMatNodeKind::OutputSurface: return "Output Surface";
        default:                          return "Invalid";
        }
    }

    namespace
    {
        DataPin makePin(const char* name, EMatValueType type, EPinDirection dir)
        {
            DataPin p{};
            p.name      = name;
            p.type      = type;
            p.direction = dir;
            return p;
        }
    } // namespace

    // ---- ConstantNode -------------------------------------------------------
    ConstantNode::ConstantNode() : Node(EMatNodeKind::Constant)
    {
        setName("Constant");
        out_pins_.push_back(makePin("out", value_type, EPinDirection::Output));
    }

    void ConstantNode::setType(EMatValueType t)
    {
        value_type = t;
        if (!out_pins_.empty())
            out_pins_[0].type = t;
    }

    // ---- InputNode ----------------------------------------------------------
    InputNode::InputNode() : Node(EMatNodeKind::Input)
    {
        setName("Input");
        out_pins_.push_back(makePin("out", inputType(input), EPinDirection::Output));
    }

    // ---- SampleTextureNode --------------------------------------------------
    SampleTextureNode::SampleTextureNode() : Node(EMatNodeKind::SampleTexture)
    {
        setName("Sample Texture");
        in_pins_.push_back(makePin("uv", EMatValueType::Vec2, EPinDirection::Input));
        out_pins_.push_back(makePin("rgba", EMatValueType::Vec4, EPinDirection::Output));
    }

    // ---- MathNode -----------------------------------------------------------
    MathNode::MathNode(EMathOp o) : Node(EMatNodeKind::Math), op(o)
    {
        setName("Math");
        in_pins_.push_back(makePin("a", EMatValueType::Float, EPinDirection::Input));
        in_pins_.push_back(makePin("b", EMatValueType::Float, EPinDirection::Input));
        out_pins_.push_back(makePin("result", EMatValueType::Float, EPinDirection::Output));
    }

    void MathNode::setOperandType(EMatValueType t)
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
    DecodeNormalNode::DecodeNormalNode() : Node(EMatNodeKind::DecodeNormal)
    {
        setName("Decode Normal");
        in_pins_.push_back(makePin("rgb", EMatValueType::Vec3, EPinDirection::Input));
        out_pins_.push_back(makePin("normal", EMatValueType::Vec3, EPinDirection::Output));
    }

    // ---- SwizzleNode --------------------------------------------------------
    SwizzleNode::SwizzleNode(EMatValueType source, EMatValueType out)
        : Node(EMatNodeKind::Swizzle), source_type(source), out_type(out)
    {
        setName("Swizzle");
        in_pins_.push_back(makePin("in", source, EPinDirection::Input));
        out_pins_.push_back(makePin("out", out, EPinDirection::Output));
    }

    void SwizzleNode::setTypes(EMatValueType source, EMatValueType out)
    {
        source_type = source;
        out_type    = out;
        if (!in_pins_.empty())  in_pins_[0].type  = source;  // Input pin type is authoritative
        if (!out_pins_.empty()) out_pins_[0].type = out;
    }

    // ---- ParamNode ----------------------------------------------------------
    ParamNode::ParamNode(EMatValueType t) : Node(EMatNodeKind::Param), type(t)
    {
        setName("Param");
        out_pins_.push_back(makePin("value", t, EPinDirection::Output));
    }

    void ParamNode::setType(EMatValueType t)
    {
        type = t;
        if (!out_pins_.empty())
            out_pins_[0].type = t;  // Output pin type is authoritative
    }

    // ---- TbnTransformNode ---------------------------------------------------
    TbnTransformNode::TbnTransformNode() : Node(EMatNodeKind::TbnTransform)
    {
        setName("TBN Transform");
        in_pins_.push_back(makePin("normal_ts", EMatValueType::Vec3, EPinDirection::Input));
        out_pins_.push_back(makePin("world_normal", EMatValueType::Vec3, EPinDirection::Output));
    }

    // ---- ConstructNode ------------------------------------------------------
    ConstructNode::ConstructNode(EMatValueType out) : Node(EMatNodeKind::Construct), out_type(out)
    {
        setName("Construct");
        const int n = (out == EMatValueType::Float) ? 1
                    : (out == EMatValueType::Vec2)  ? 2
                    : (out == EMatValueType::Vec3)  ? 3
                                                    : 4;
        static const char* const kComp[4] = { "x", "y", "z", "w" };
        for (int i = 0; i < n; ++i)
            in_pins_.push_back(makePin(kComp[i], EMatValueType::Float, EPinDirection::Input));
        out_pins_.push_back(makePin("out", out, EPinDirection::Output));
    }

    // ---- OutputSurfaceNode --------------------------------------------------
    // One input pin per EMaterialAttribute; name/type/default come from the
    // contract table, and pin index i corresponds directly to attribute i
    // (so lowering can map between them with no extra lookup).
    OutputSurfaceNode::OutputSurfaceNode() : Node(EMatNodeKind::OutputSurface)
    {
        setName("Output Surface");
        for (const auto& a : kMaterialAttributes)
        {
            DataPin pin = makePin(a.glsl_name, a.type, EPinDirection::Input);
            pin.constant[0] = a.dflt[0];
            pin.constant[1] = a.dflt[1];
            pin.constant[2] = a.dflt[2];
            pin.constant[3] = a.dflt[3];
            in_pins_.push_back(std::move(pin));
        }
    }

} // namespace lux::rdesc
