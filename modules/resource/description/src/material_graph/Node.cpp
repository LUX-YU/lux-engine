// =============================================================================
//  Node.cpp  —  Node 导出锚点 + 具体节点构造（按契约建立引脚）
// =============================================================================

#include <lux/engine/description/material_graph/Node.hpp>
#include <lux/engine/description/material_graph/Nodes.hpp>

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
            in_pins_[0].type = t;  // 输入引脚类型权威 —— resolveInput 据此类型检查
            in_pins_[1].type = t;
        }
        if (!out_pins_.empty())
            out_pins_[0].type = t;  // 仅提示；lowering 对 Dot 等会推导出 Float
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
        if (!in_pins_.empty())  in_pins_[0].type  = source;  // 输入引脚类型权威
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
            out_pins_[0].type = t;  // 输出引脚类型权威
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
    // 每个 EMaterialAttribute 一个输入引脚，名字/类型/默认值取自契约表，
    // 引脚下标 i 即对应属性 i（便于 lowering 直接映射）。
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
