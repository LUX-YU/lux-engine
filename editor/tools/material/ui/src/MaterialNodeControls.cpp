#include <lux/engine/editor/material/MaterialNodeControls.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <imgui.h>

namespace lux::editor::material
{
    std::unique_ptr<lux::material::Node> makeMaterialNode(lux::material::EMatNodeKind kind)
    {
        using namespace lux::material;
        switch (kind)
        {
        case EMatNodeKind::CONSTANT:
            return std::make_unique<ConstantNode>();
        case EMatNodeKind::INPUT:
            return std::make_unique<InputNode>();
        case EMatNodeKind::SAMPLE_TEXTURE:
            return std::make_unique<SampleTextureNode>();
        case EMatNodeKind::MATH:
            return std::make_unique<MathNode>();
        case EMatNodeKind::SWIZZLE:
            return std::make_unique<SwizzleNode>();
        case EMatNodeKind::CONSTRUCT:
            return std::make_unique<ConstructNode>();
        case EMatNodeKind::DECODE_NORMAL:
            return std::make_unique<DecodeNormalNode>();
        case EMatNodeKind::TBN_TRANSFORM:
            return std::make_unique<TbnTransformNode>();
        case EMatNodeKind::PARAM:
            return std::make_unique<ParamNode>();
        case EMatNodeKind::OUTPUT_SURFACE:
            return std::make_unique<OutputSurfaceNode>();
        default:
            return {};
        }
    }
    bool editMaterialValueType(const char* label, lux::material::EValueType& value)
    {
        int selected = static_cast<int>(value);
        if (!ImGui::Combo(label, &selected, "Float\0Vec2\0Vec3\0Vec4\0"))
        {
            return false;
        }
        value = static_cast<lux::material::EValueType>(selected);
        return true;
    }

    template <class Slots> static bool slotChoice(const char* label, std::uint32_t& value, const Slots& slots)
    {
        bool changed{};
        const char* current = value < slots.size() ? slots[value].name.c_str() : "Unassigned";
        if (ImGui::BeginCombo(label, current))
        {
            for (std::size_t index{}; index < slots.size(); ++index)
            {
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::Selectable(slots[index].name.c_str(), index == value))
                {
                    value = static_cast<std::uint32_t>(index);
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool editMaterialNodePayload(
        lux::material::Node& node,
        std::span<const lux::material::TextureSlotDecl> textures,
        std::span<const lux::material::ParamSlotDecl> parameters
    )
    {
        using namespace lux::material;
        bool changed{};
        if (auto* constant = node.as<ConstantNode>())
        {
            auto type = constant->value_type;
            if (editMaterialValueType("Type", type))
            {
                constant->setType(type);
                changed = true;
            }
            changed |= ImGui::DragScalarN(
                "Value",
                ImGuiDataType_Float,
                constant->value,
                static_cast<int>(constant->value_type) + 1,
                0.01F
            );
        }
        else if (auto* input = node.as<InputNode>())
        {
            const auto* description = materialInputDescription(input->input);
            if (ImGui::BeginCombo("Input", description ? description->name : "Unassigned"))
            {
                for (const auto& candidate : kMaterialInputs)
                {
                    if (ImGui::Selectable(candidate.name, input->input == candidate.input))
                    {
                        input->setInput(candidate.input);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
        }
        else if (auto* sample = node.as<SampleTextureNode>())
        {
            changed |= slotChoice("Texture slot", sample->texture_slot, textures);
        }
        else if (auto* parameter = node.as<ParamNode>())
        {
            const auto& slots = parameters;
            if (slotChoice("Parameter slot", parameter->param_slot, slots))
            {
                parameter->setType(slots[parameter->param_slot].type);
                changed = true;
            }
            auto type = parameter->type;
            if (editMaterialValueType("Parameter type", type))
            {
                parameter->setType(type);
                changed = true;
            }
        }
        else if (auto* math = node.as<MathNode>())
        {
            constexpr const char* names[]{
                "Multiply",
                "Add",
                "Subtract",
                "Divide",
                "Dot",
                "Minimum",
                "Maximum",
                "Power",
                "Step",
                "Modulo",
                "Cross",
                "Reflect",
                "Lerp (requires three inputs)",
                "Saturate",
                "One minus",
                "Absolute",
                "Square root",
                "Floor",
                "Fraction",
                "Sine",
                "Cosine",
                "Normalize",
                "Length"
            };
            static_assert(std::size(names) == static_cast<std::size_t>(EMathOp::LENGTH) + 1);
            int operation = static_cast<int>(math->op);
            if (ImGui::Combo("Operation", &operation, names, static_cast<int>(std::size(names))))
            {
                math->op = static_cast<EMathOp>(operation);
                changed = true;
            }
            auto type = math->operand_type;
            if (editMaterialValueType("Operand type", type))
            {
                math->setOperandType(type);
                changed = true;
            }
        }
        else if (auto* swizzle = node.as<SwizzleNode>())
        {
            auto input = swizzle->source_type;
            auto output = swizzle->out_type;
            const bool input_changed = editMaterialValueType("Input type", input);
            const bool output_changed = editMaterialValueType("Output type", output);
            if (input_changed || output_changed)
            {
                swizzle->setTypes(input, output);
                changed = true;
            }
            for (int index{}; index <= static_cast<int>(swizzle->out_type); ++index)
            {
                ImGui::PushID(index);
                int channel = swizzle->components[index];
                if (ImGui::Combo("Channel", &channel, "X\0Y\0Z\0W\0"))
                {
                    swizzle->components[index] = static_cast<std::uint8_t>(channel);
                    changed = true;
                }
                ImGui::PopID();
            }
        }
        else if (auto* construct = node.as<ConstructNode>())
        {
            auto type = construct->out_type;
            if (editMaterialValueType("Output type", type))
            {
                construct->setType(type);
                changed = true;
            }
        }
        for (std::size_t index{}; index < node.inputs().size(); ++index)
        {
            auto& pin = node.inputs()[index];
            ImGui::PushID(static_cast<int>(index));
            changed |= ImGui::DragScalarN(
                pin.name.c_str(),
                ImGuiDataType_Float,
                pin.constant,
                static_cast<int>(pin.type) + 1,
                0.01F
            );
            ImGui::PopID();
        }
        return changed;
    }

}
