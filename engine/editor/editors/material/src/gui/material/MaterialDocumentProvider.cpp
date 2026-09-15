#include <lux/engine/editor/gui/PublicationControls.hpp>
#include <algorithm>
#include <imgui.h>
#include <imgui_node_editor.h>
#include <imgui_stdlib.h>
#include <lux/engine/editor/gui/DocumentPane.hpp>
#include <lux/engine/editor/gui/material/MaterialDocumentProvider.hpp>
#include <lux/engine/editor/gui/asset/AssetReference.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/editor/project/Project.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <map>
#include <unordered_map>

namespace lux::editor::gui
{
namespace
{
namespace canvas = ax::NodeEditor;
struct CanvasDelete final
{
    void operator()(canvas::EditorContext *context) const noexcept
    {
        canvas::DestroyEditor(context);
    }
};
struct CanvasScope final
{
    canvas::EditorContext *previous{canvas::GetCurrentEditor()};
    explicit CanvasScope(canvas::EditorContext *context)
    {
        canvas::SetCurrentEditor(context);
    }
    ~CanvasScope()
    {
        canvas::SetCurrentEditor(previous);
    }
};
std::unique_ptr<canvas::EditorContext, CanvasDelete> createCanvas()
{
    canvas::Config config;
    config.SettingsFile = nullptr;
    return std::unique_ptr<canvas::EditorContext, CanvasDelete>(canvas::CreateEditor(&config));
}
std::unique_ptr<lux::material::Node> createNode(lux::material::EMatNodeKind kind)
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

class MaterialPane final : public DocumentPane<MaterialPane, material::MaterialEditor>
{
    struct Position final
    {
        lux::graph::GraphNodeLayout author, display;
    };
    struct ConstantGesture final
    {
        lux::material::NodeId node;
        editing::StateId base;
        std::array<float, 4> value;
    };
    struct Idle final
    {
    };
    struct NodeDraft final
    {
        editing::StateId base;
        std::unique_ptr<lux::material::Node> value;
        std::string name;
        bool changed{};
    };
    template <class Slot> struct SlotDraft final
    {
        editing::StateId base;
        std::vector<Slot> values;
        bool changed{};
    };

  public:
    MaterialPane(material::MaterialEditor &document, std::string id)
        : DocumentPane(document, std::move(id), "Material"), canvas_(createCanvas()), name_(document.source().name),
          content_connection_(document.observeScoped<material::MaterialEditor::contentChanged>(
              [this](editing::Revision) noexcept { source_changed_ = true; }))
    {
    }

    void poll(PollBudget &budget) override
    {
        publication_.poll(document_);
        DocumentPane::poll(budget);
    }

  private:
    PublicationControls publication_;

    template <class Result> bool accept(const Result &result)
    {
        if (result)
        {
            error_.clear();
            return true;
        }
        if constexpr (std::same_as<typename Result::error_type, editing::EditFailure>)
        {
            error_ = result.error().message.data();
            if (error_.empty())
            {
                error_ = "Edit rejected: " + std::to_string(static_cast<unsigned>(result.error().code));
            }
        }
        else
        {
            error_ = result.error().domain + ":" + std::to_string(result.error().reason) + " " + result.error().message;
        }
        return false;
    }
    bool unchanged(editing::StateId state)
    {
        const auto history = document_.historyView();
        if (!history || history->history.current != state)
        {
            error_ = "The material changed during this gesture. Start the edit again.";
            return false;
        }
        return true;
    }
    void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context) override
    {
        context.activateContext(lux::ui::UiContextIdView{id()});
        if (!canvas_)
        {
            frame.text("Node canvas could not be created");
            return;
        }
        CanvasScope scope(canvas_.get());
        const auto history = document_.historyView();
        if (!history)
        {
            accept(history);
            return;
        }
        ImGui::BeginDisabled(!document_.project().writable());
        if (source_changed_ && !name_active_)
        {
            name_ = document_.source().name;
        }
        ImGui::SetNextItemWidth(240);
        ImGui::InputText("Name", &name_);
        if (ImGui::IsItemActivated())
        {
            name_base_ = history->history.current;
            name_active_ = true;
        }
        if (ImGui::IsItemDeactivated())
        {
            if (ImGui::IsItemDeactivatedAfterEdit() && unchanged(name_base_))
            {
                accept(document_.rename(name_));
            }
            name_active_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add node"))
        {
            ImGui::OpenPopup("node-palette");
        }
        if (ImGui::BeginPopup("node-palette"))
        {
            for (unsigned i = 1; i < static_cast<unsigned>(lux::material::EMatNodeKind::COUNT); ++i)
            {
                const auto kind = static_cast<lux::material::EMatNodeKind>(i);
                if (ImGui::Selectable(lux::material::toString(kind)))
                {
                    auto node = createNode(kind);
                    accept(document_.insertNode(node));
                }
            }
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Compile"))
        {
            bool ready = true;
            if (compile_.serial)
            {
                ready = accept(document_.acknowledgeCompile(compile_));
            }
            if (ready)
            {
                auto request = document_.requestCompile();
                if (accept(request))
                {
                    compile_ = *request;
                }
            }
        }
        if (compile_.serial)
        {
            auto status = document_.compileStatus(compile_);
            if (status)
            {
                if (std::holds_alternative<material::MaterialCompilePending>(*status))
                {
                    frame.textMuted("Compiling...");
                }
                else if (const auto *success = std::get_if<material::MaterialCompileSucceeded>(&*status))
                {
                    frame.textMuted(success->current ? "Compilation succeeded"
                                                     : "Compiled result belongs to an earlier edit");
                    ImGui::BeginDisabled(!success->current || !document_.project().writable() ||
                                         !document_.saveRequests().empty());
                    if (ImGui::SmallButton("Save source and publish asset"))
                    {
                        const auto publication = document_.requestPublish(compile_, "desktop");
                        if (accept(publication))
                        {
                            publication_.track(*publication);
                        }
                    }
                    ImGui::EndDisabled();
                }
                else
                {
                    const auto &failure = std::get<material::MaterialCompileFailed>(*status).failure;
                    frame.text(failure.message);
                }
            }
        }
        publication_.draw(document_, frame);
        drawSettings(history->history.current);
        drawProperties(history->history.current);
        if (!error_.empty())
        {
            frame.text(error_);
        }
        source_changed_ = false;
        drawCanvas();
    }
    void drawCanvas()
    {
        const auto &graph = document_.source().graph;
        const bool writable = document_.project().writable();
        canvas::Begin("Material graph");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            move_base_ = document_.historyView()->history.current;
        }
        std::size_t ordinal{};
        for (const auto &record : graph.topology().nodes())
        {
            const auto *node = graph.node(record.id);
            const auto *author = graph.layout().find(record.id);
            const auto layout = author ? *author : lux::graph::GraphNodeLayout{};
            auto [position, inserted] = positions_.try_emplace(record.id);
            if (inserted || position->second.author != layout)
            {
                position->second.author = layout;
                position->second.display = layout.placed ? layout
                                                         : lux::graph::GraphNodeLayout{float(ordinal % 4) * 240,
                                                                                       float(ordinal / 4) * 280, true};
                canvas::SetNodePosition(canvas::NodeId{record.id.value},
                                        {position->second.display.x, position->second.display.y});
            }
            ++ordinal;
            canvas::BeginNode(canvas::NodeId{record.id.value});
            ImGui::TextUnformatted(node->name().empty() ? lux::material::toString(node->kind()) : node->name().c_str());
            for (const auto &pin : node->inputs())
            {
                canvas::BeginPin(canvas::PinId{pin.id.value}, canvas::PinKind::Input);
                ImGui::Text("< %s", pin.name.c_str());
                canvas::EndPin();
            }
            if (const auto *constant = node->as<lux::material::ConstantNode>())
            {
                std::array<float, 4> value;
                std::ranges::copy(constant->value, value.begin());
                if (auto *gesture = std::get_if<ConstantGesture>(&constant_); gesture && gesture->node == record.id)
                {
                    value = gesture->value;
                }
                ImGui::PushID(static_cast<int>(record.id.value));
                ImGui::BeginDisabled(!writable);
                ImGui::SetNextItemWidth(220);
                ImGui::DragFloat4("##value", value.data(), 0.01F);
                if (ImGui::IsItemActivated())
                {
                    constant_.emplace<ConstantGesture>(record.id, document_.historyView()->history.current, value);
                }
                if (auto *gesture = std::get_if<ConstantGesture>(&constant_); gesture && gesture->node == record.id)
                {
                    gesture->value = value;
                    if (ImGui::IsItemDeactivated())
                    {
                        if (ImGui::IsItemDeactivatedAfterEdit() && unchanged(gesture->base))
                        {
                            accept(document_.setConstant(record.id, gesture->value));
                        }
                        constant_.emplace<Idle>();
                    }
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            for (const auto &pin : node->outputs())
            {
                canvas::BeginPin(canvas::PinId{pin.id.value}, canvas::PinKind::Output);
                ImGui::Text("%s >", pin.name.c_str());
                canvas::EndPin();
            }
            canvas::EndNode();
        }
        const auto links = graph.topology().links();
        for (const auto &link : links)
        {
            const auto key = std::pair{link.from.value, link.to.value};
            auto [found, inserted] = links_.try_emplace(key, next_link_);
            if (inserted)
            {
                ++next_link_;
            }
            canvas::Link(canvas::LinkId{found->second}, canvas::PinId{link.from.value}, canvas::PinId{link.to.value});
        }
        lux::graph::LinkRecord created;
        if (writable && canvas::BeginCreate())
        {
            canvas::PinId first, second;
            if (canvas::QueryNewLink(&first, &second) && first && second && canvas::AcceptNewItem())
            {
                created = {{first.Get()}, {second.Get()}};
                const auto *pin = graph.topology().findPin(created.from);
                if (pin && pin->direction == lux::graph::EPinDirection::INPUT)
                {
                    std::swap(created.from, created.to);
                }
            }
            canvas::EndCreate();
        }
        std::vector<lux::material::NodeId> removed_nodes;
        std::vector<lux::graph::LinkRecord> removed_links;
        if (writable && canvas::BeginDelete())
        {
            canvas::NodeId node;
            while (canvas::QueryDeletedNode(&node))
            {
                if (canvas::AcceptDeletedItem())
                {
                    removed_nodes.push_back(lux::material::NodeId{node.Get()});
                }
            }
            canvas::LinkId link;
            while (canvas::QueryDeletedLink(&link))
            {
                const auto found =
                    std::ranges::find_if(links_, [&](const auto &value) { return value.second == link.Get(); });
                if (found != links_.end() && canvas::AcceptDeletedItem())
                {
                    removed_links.push_back({{found->first.first}, {found->first.second}});
                }
            }
            canvas::EndDelete();
        }
        canvas::End();
        if (!removed_nodes.empty() || !removed_links.empty())
        {
            accept(document_.removeNodes(removed_nodes, removed_links));
        }
        if (created.from.valid() && created.to.valid())
        {
            accept(document_.connect(created.from, created.to));
        }
        if (writable && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            std::vector<lux::graph::GraphLayoutEntry> moved;
            for (auto &[node, position] : positions_)
            {
                if (!graph.node(node))
                {
                    continue;
                }
                const auto value = canvas::GetNodePosition(canvas::NodeId{node.value});
                if (value.x != position.display.x || value.y != position.display.y)
                {
                    moved.push_back({node, {value.x, value.y, true}});
                }
            }
            if (!moved.empty() && (!unchanged(move_base_) || !accept(document_.moveNodes(moved))))
            {
                for (const auto &item : moved)
                {
                    const auto &position = positions_.at(item.node).display;
                    canvas::SetNodePosition(canvas::NodeId{item.node.value}, {position.x, position.y});
                }
            }
        }
        std::erase_if(positions_, [&](const auto &item) { return !graph.node(item.first); });
        std::erase_if(links_, [&](const auto &item) {
            return !graph.topology().findLink(lux::graph::PinId{item.first.first},
                                              lux::graph::PinId{item.first.second});
        });
    }

    static bool valueType(const char *label, lux::material::EValueType &value)
    {
        int selected = static_cast<int>(value);
        if (!ImGui::Combo(label, &selected, "Float\0Vec2\0Vec3\0Vec4\0"))
        {
            return false;
        }
        value = static_cast<lux::material::EValueType>(selected);
        return true;
    }

    template <class Slots> static bool slotChoice(const char *label, std::uint32_t &value, const Slots &slots)
    {
        bool changed{};
        const char *current = value < slots.size() ? slots[value].name.c_str() : "Unassigned";
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

    bool drawPayload(lux::material::Node &node)
    {
        using namespace lux::material;
        bool changed{};
        if (auto *constant = node.as<ConstantNode>())
        {
            auto type = constant->value_type;
            if (valueType("Type", type))
            {
                constant->setType(type);
                changed = true;
            }
            changed |= ImGui::DragScalarN("Value", ImGuiDataType_Float, constant->value,
                                          static_cast<int>(constant->value_type) + 1, 0.01F);
        }
        else if (auto *input = node.as<InputNode>())
        {
            const auto *description = materialInputDescription(input->input);
            if (ImGui::BeginCombo("Input", description ? description->name : "Unassigned"))
            {
                for (const auto &candidate : kMaterialInputs)
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
        else if (auto *sample = node.as<SampleTextureNode>())
        {
            changed |= slotChoice("Texture slot", sample->texture_slot, document_.source().graph.texture_slots);
        }
        else if (auto *parameter = node.as<ParamNode>())
        {
            const auto &slots = document_.source().graph.param_slots;
            if (slotChoice("Parameter slot", parameter->param_slot, slots))
            {
                parameter->setType(slots[parameter->param_slot].type);
                changed = true;
            }
            auto type = parameter->type;
            if (valueType("Parameter type", type))
            {
                parameter->setType(type);
                changed = true;
            }
        }
        else if (auto *math = node.as<MathNode>())
        {
            constexpr const char *names[]{"Multiply",
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
                                          "Length"};
            static_assert(std::size(names) == static_cast<std::size_t>(EMathOp::LENGTH) + 1);
            int operation = static_cast<int>(math->op);
            if (ImGui::Combo("Operation", &operation, names, static_cast<int>(std::size(names))))
            {
                math->op = static_cast<EMathOp>(operation);
                changed = true;
            }
            auto type = math->operand_type;
            if (valueType("Operand type", type))
            {
                math->setOperandType(type);
                changed = true;
            }
        }
        else if (auto *swizzle = node.as<SwizzleNode>())
        {
            auto input = swizzle->source_type;
            auto output = swizzle->out_type;
            const bool input_changed = valueType("Input type", input);
            const bool output_changed = valueType("Output type", output);
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
        else if (auto *construct = node.as<ConstructNode>())
        {
            auto type = construct->out_type;
            if (valueType("Output type", type))
            {
                construct->setType(type);
                changed = true;
            }
        }
        for (std::size_t index{}; index < node.inputs().size(); ++index)
        {
            auto &pin = node.inputs()[index];
            ImGui::PushID(static_cast<int>(index));
            changed |= ImGui::DragScalarN(pin.name.c_str(), ImGuiDataType_Float, pin.constant,
                                          static_cast<int>(pin.type) + 1, 0.01F);
            ImGui::PopID();
        }
        return changed;
    }

    template <class Slot> void drawSlots(SlotDraft<Slot> &draft, editing::StateId current)
    {
        using namespace lux::material;
        constexpr bool textures = std::same_as<Slot, TextureSlotDecl>;
        const auto &values = [&]() -> const std::vector<Slot> & {
            if constexpr (textures)
            {
                return document_.source().graph.texture_slots;
            }
            else
            {
                return document_.source().graph.param_slots;
            }
        }();
        if (!draft.changed && draft.base != current)
        {
            draft.base = current;
            draft.values = values;
        }
        ImGui::PushID(textures ? "texture-slots" : "parameter-slots");
        for (std::size_t index{}; index < draft.values.size(); ++index)
        {
            auto &slot = draft.values[index];
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::TreeNodeEx("slot", ImGuiTreeNodeFlags_DefaultOpen, "Slot %zu", index))
            {
                draft.changed |= ImGui::InputText("Name", &slot.name);
                if constexpr (textures)
                {
                    const auto picked =
                        drawAssetReference(document_.project(), slot.texture, lux::asset::TextureAsset::primary_magic);
                    if (!picked)
                    {
                        static_cast<void>(accept(picked));
                    }
                    else if (*picked)
                    {
                        draft.changed = true;
                    }
                }
                else
                {
                    draft.changed |= valueType("Type", slot.type);
                    draft.changed |= ImGui::DragScalarN("Default", ImGuiDataType_Float, slot.dflt,
                                                        static_cast<int>(slot.type) + 1, 0.01F);
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::BeginDisabled(draft.values.size() >= MaterialSourceLimits{}.max_slots);
        if (ImGui::SmallButton("Add slot"))
        {
            auto &slot = draft.values.emplace_back();
            slot.name = (textures ? "Texture " : "Parameter ") + std::to_string(draft.values.size());
            draft.changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(draft.values.empty());
        if (ImGui::SmallButton("Remove last slot"))
        {
            draft.values.pop_back();
            draft.changed = true;
        }
        ImGui::EndDisabled();
        if (ImGui::Button("Apply slots"))
        {
            const auto result = [&] {
                if constexpr (textures)
                {
                    return document_.setTextureSlots(draft.base, draft.values);
                }
                else
                {
                    return document_.setParameterSlots(draft.base, draft.values);
                }
            }();
            if (accept(result))
            {
                draft.changed = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert slots"))
        {
            draft.values = values;
            draft.base = current;
            draft.changed = false;
        }
        ImGui::PopID();
    }

    void drawSettings(editing::StateId current)
    {
        if (!ImGui::CollapsingHeader("Material settings"))
        {
            return;
        }
        ImGui::BeginChild("material-settings", {0, 240}, ImGuiChildFlags_Borders);
        ImGui::BeginDisabled(!document_.project().writable());
        if (!state_changed_ && state_base_ != current)
        {
            render_state_ = document_.source().graph.render_state;
            state_base_ = current;
        }
        int shading_model = static_cast<int>(document_.source().graph.shading_model);
        if (ImGui::Combo("Shading model", &shading_model,
                         "Unlit\0Legacy lit\0PBR metallic / roughness\0Stylized\0Graph\0"))
        {
            accept(document_.setShadingModel(static_cast<lux::rdesc::ELightingTechnique>(shading_model)));
        }
        int alpha_mode = static_cast<int>(render_state_.alpha_mode);
        if (ImGui::Combo("Alpha mode", &alpha_mode, "Opaque\0Mask\0Blend\0"))
        {
            render_state_.alpha_mode = static_cast<lux::rdesc::EAlphaMode>(alpha_mode);
            state_changed_ = true;
        }
        state_changed_ |= ImGui::SliderFloat("Cutoff", &render_state_.alpha_cutoff, 0, 1);
        state_changed_ |= ImGui::Checkbox("Double sided", &render_state_.double_sided);
        if (ImGui::Button("Apply render state") && unchanged(state_base_))
        {
            if (accept(document_.setRenderState(render_state_)))
            {
                state_changed_ = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert render state"))
        {
            render_state_ = document_.source().graph.render_state;
            state_base_ = current;
            state_changed_ = false;
        }
        if (ImGui::TreeNode("Texture slots"))
        {
            drawSlots(textures_, current);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Parameter slots"))
        {
            drawSlots(parameters_, current);
            ImGui::TreePop();
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
    }

    void drawProperties(editing::StateId current)
    {
        if (!ImGui::CollapsingHeader("Node properties"))
        {
            return;
        }
        std::array<canvas::NodeId, 2> selected;
        if (canvas::GetSelectedNodes(selected.data(), static_cast<int>(selected.size())) != 1)
        {
            ImGui::TextDisabled("Select a single node to edit its properties");
            return;
        }
        const auto *node = document_.source().graph.node(lux::material::NodeId{selected.front().Get()});
        if (!node)
        {
            draft_.emplace<Idle>();
            return;
        }
        auto *draft = std::get_if<NodeDraft>(&draft_);
        if (!draft || draft->value->id() != node->id() || (!draft->changed && draft->base != current))
        {
            draft = &draft_.emplace<NodeDraft>(current, node->clone(), node->name(), false);
        }
        ImGui::BeginChild("node-properties", {0, 230}, ImGuiChildFlags_Borders);
        ImGui::BeginDisabled(!document_.project().writable());
        if (ImGui::InputText("Node name", &draft->name))
        {
            draft->value->setName(draft->name);
            draft->changed = true;
        }
        draft->changed |= drawPayload(*draft->value);
        const bool apply = ImGui::Button("Apply properties");
        ImGui::SameLine();
        const bool revert = ImGui::Button("Revert properties");
        if (apply && accept(document_.replaceNode(draft->base, draft->value)))
        {
            draft_.emplace<Idle>();
        }
        if (revert)
        {
            draft_.emplace<Idle>();
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
    }
    std::unique_ptr<canvas::EditorContext, CanvasDelete> canvas_;
    std::unordered_map<lux::material::NodeId, Position> positions_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> links_;
    std::uint64_t next_link_{1};
    std::variant<Idle, ConstantGesture> constant_;
    std::variant<Idle, NodeDraft> draft_;
    SlotDraft<lux::material::TextureSlotDecl> textures_;
    SlotDraft<lux::material::ParamSlotDecl> parameters_;
    lux::material::RenderState render_state_;
    editing::StateId state_base_;
    bool state_changed_{};
    material::MaterialCompileId compile_;
    std::string name_, error_;
    editing::StateId name_base_, move_base_;
    bool source_changed_{true}, name_active_{};
    object::ScopedConnection content_connection_;
};
} // namespace

GuiDocumentProvider materialDocumentProvider()
{
    return {std::string(material::kMaterialDocumentType),
            [](const ProjectAssetEntry &asset) { return asset.kind == EProjectAssetKind::MATERIAL_GRAPH; },
            [](Editor &editor, process::ExecutionRuntime &runtime, rendering::EditorRenderer &) -> EditorResult<void> {
                return editor.registerDocument({std::string(material::kMaterialDocumentType),
                                                [&runtime](Project &project, const OpenDocumentRequest &request) {
                                                    return material::openMaterialDocument(project, request, runtime);
                                                }});
            },
            [](DocumentEditor &base, EditorWindow &window, rendering::EditorRenderer &,
               process::ExecutionRuntime &) -> EditorResult<void> {
                auto *document = dynamic_cast<material::MaterialEditor *>(&base);
                if (!document)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "material.gui"});
                }
                auto pane = std::make_unique<MaterialPane>(*document,
                                                           "material-" + std::to_string(document->historyId().value));
                auto attached = pane->attach(window.uiSession());
                if (!attached)
                {
                    pane->requestClose();
                    return attached;
                }
                std::vector<std::unique_ptr<DocumentView>> batch;
                batch.push_back(std::move(pane));
                auto adopted = document->addViews(batch);
                if (!adopted)
                {
                    for (const auto &view : batch)
                    {
                        view->requestClose();
                    }
                }
                return adopted;
            }};
}
} // namespace lux::editor::gui
