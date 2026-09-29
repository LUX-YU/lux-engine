#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/material/MaterialEditorImpl.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/ui/AssetActions.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <algorithm>
#include <lux/engine/editor/ui/material/MaterialPreviewElement.hpp>
#include <imgui.h>
#include <imgui_node_editor.h>
#include <imgui_stdlib.h>
#include <lux/engine/editor/ui/NodeCanvasIds.hpp>
#include <lux/engine/editor/ui/PublicationControls.hpp>
#include <lux/engine/editor/ui/asset/AssetPickerElement.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <map>
#include <unordered_map>

namespace lux::editor::material
{
    using namespace lux::editor::ui;
    namespace
    {
        namespace canvas = ax::NodeEditor;
        struct CanvasDelete final
        {
            void operator()(canvas::EditorContext* context) const noexcept
            {
                canvas::DestroyEditor(context);
            }
        };
        struct CanvasScope final
        {
            canvas::EditorContext* previous{canvas::GetCurrentEditor()};
            explicit CanvasScope(canvas::EditorContext* context)
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
            // Pane layout changes must not magnify a canvas first opened with little available height.
            config.CanvasSizeMode = canvas::CanvasSizeMode::CenterOnly;
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

    } // namespace

    class MaterialEditor::Impl::GraphElement final : public lux::ui::Element
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
        {};
        struct NodeDraft final
        {
            editing::StateId base;
            std::unique_ptr<lux::material::Node> value;
            std::string name;
            bool changed{};
        };
        template <class Slot> struct TSlotDraft final
        {
            editing::StateId base;
            std::vector<Slot> values;
            bool changed{};
        };

    public:
        GraphElement(lux::ui::Element& parent, Impl& editor_, EditorResult<void>& status)
            : lux::ui::Element(parent, lux::ui::ElementId{"graph"}), editor_(editor_), canvas_(createCanvas()),
              name_(editor_.source().name), content_connection_(lux::editor::detail::takeConnection(
                                                lux::object::LuxObject::connect(
                                                    editor_.editor_,
                                                    &material::MaterialEditor::contentChanged,
                                                    [this](editing::Revision) noexcept { source_changed_ = true; }
                                                ),
                                                status
                                            ))
        {
            synchronizePickers(status);
        }

        [[nodiscard]] EditorResult<void> finishEditing()
        {
            const auto history_ = editor_.historyView();
            if (!history_)
                return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "graph.history", 0, {}, history_.error()}
                );
            const auto initial = history_->history.current;
            const auto stale = lux::cxx::unexpected(EditorFailure{
                EEditorError::BUSY,
                "graph.interaction",
                0,
                "The editor changed while this interaction was active. Resolve or cancel the retained draft."
            });
            if (name_active_ && name_base_ != initial)
                return stale;
            if (auto* gesture = std::get_if<ConstantGesture>(&constant_); gesture && gesture->base != initial)
                return stale;
            if (auto* draft = std::get_if<NodeDraft>(&draft_); draft && draft->changed && draft->base != initial)
                return stale;
            if ((textures_.changed && textures_.base != initial) ||
                (parameters_.changed && parameters_.base != initial) || (state_changed_ && state_base_ != initial))
                return stale;
            std::vector<lux::graph::GraphLayoutEntry> moved;
            auto* previous = canvas::GetCurrentEditor();
            canvas::SetCurrentEditor(canvas_.get());
            for (const auto& [node, position] : positions_)
            {
                if (!(editor_.source().graph.node(node)))
                    continue;
                const auto value = canvas::GetNodePosition(canvas_ids_.node(node.value));
                if (value.x != position.display.x || value.y != position.display.y)
                    moved.push_back({node, {value.x, value.y, true}});
            }
            canvas::SetCurrentEditor(previous);
            if (!moved.empty() && move_base_ != initial)
                return stale;
            auto before = initial;
            const auto adopt = [&](const auto& result) -> EditorResult<void> {
                if (!result)
                    return lux::cxx::unexpected(
                        EditorFailure{EEditorError::INVALID_STATE, "graph.finish-editing", 0, {}, result.error()}
                    );
                const auto next = editor_.historyView()->history.current;
                if (name_base_ == before)
                    name_base_ = next;
                if (move_base_ == before)
                    move_base_ = next;
                if (auto* gesture = std::get_if<ConstantGesture>(&constant_); gesture && gesture->base == before)
                    gesture->base = next;
                if (auto* draft = std::get_if<NodeDraft>(&draft_); draft && draft->base == before)
                    draft->base = next;
                if (textures_.base == before)
                    textures_.base = next;
                if (parameters_.base == before)
                    parameters_.base = next;
                if (state_base_ == before)
                    state_base_ = next;
                before = next;
                return {};
            };
            if (name_active_)
            {
                if (auto result = adopt(editor_.rename(name_)); !result)
                    return result;
                name_active_ = false;
            }
            if (auto* gesture = std::get_if<ConstantGesture>(&constant_))
            {
                if (auto result = adopt(editor_.setConstant(gesture->node, gesture->value)); !result)
                    return result;
                constant_.emplace<Idle>();
            }
            if (auto* draft = std::get_if<NodeDraft>(&draft_); draft && draft->changed)
            {
                if (auto result = adopt(editor_.replaceNode(draft->base, draft->value)); !result)
                    return result;
                draft_.emplace<Idle>();
            }
            if (textures_.changed)
            {
                if (auto result = adopt(editor_.setTextureSlots(textures_.base, textures_.values)); !result)
                    return result;
                textures_.changed = false;
            }
            if (parameters_.changed)
            {
                if (auto result = adopt(editor_.setParameterSlots(parameters_.base, parameters_.values)); !result)
                    return result;
                parameters_.changed = false;
            }
            if (state_changed_)
            {
                if (auto result = adopt(editor_.setRenderState(render_state_)); !result)
                    return result;
                state_changed_ = false;
            }
            if (!moved.empty())
            {
                if (auto result = adopt(editor_.moveNodes(moved)); !result)
                    return result;
                for (const auto& move : moved)
                    positions_.at(move.node) = {move.layout, move.layout};
            }
            root().releaseFocus(*this);
            return {};
        }

        void update() noexcept override
        {
            if (observed_history_ != editor_.historyId())
            {
                observed_history_ = editor_.historyId();
                auto* previous = canvas::GetCurrentEditor();
                canvas::SetCurrentEditor(canvas_.get());
                canvas::ClearSelection();
                canvas::SetCurrentEditor(previous);
                canvas_ids_ = {};
                positions_.clear();
                links_.clear();
                next_link_ = 1;
                constant_.emplace<Idle>();
                draft_.emplace<Idle>();
                textures_ = {};
                parameters_ = {};
                texture_pickers_.clear();
                state_changed_ = name_active_ = false;
                source_changed_ = true;
                name_ = editor_.source().name;
                error_.clear();
            }
            EditorResult<void> status;
            synchronizePickers(status);
            if (!status)
                static_cast<void>(accept(status));
            if (const auto history_ = editor_.historyView();
                history_ && history_->history.revision != observed_revision_)
            {
                observed_revision_ = history_->history.revision;
                source_changed_ = true;
            }
        }

    private:
        // Texture slot controls are built only during maintenance. Slot drafts remain bounded
        // by MaterialSourceLimits and retain their own values until Apply/finishEditing.
        struct TexturePicker final
        {
            std::unique_ptr<AssetPickerElement> element;
            object::Connection connection;
        };
        void synchronizePickers(EditorResult<void>& status)
        {
            const auto current = editor_.historyView();
            if (current && !textures_.changed && textures_.base != current->history.current)
            {
                textures_.base = current->history.current;
                textures_.values = editor_.source().graph.texture_slots;
            }
            texture_pickers_.resize(std::min(texture_pickers_.size(), textures_.values.size()));
            while (texture_pickers_.size() < textures_.values.size())
            {
                const auto index = texture_pickers_.size();
                TexturePicker picker;
                picker.element = std::make_unique<AssetPickerElement>(
                    *this,
                    lux::ui::ElementId{"texture-slot-" + std::to_string(index)},
                    &editor_.project(),
                    lux::asset::TextureAsset::primary_magic,
                    textures_.values[index].texture
                );
                auto* element = picker.element.get();
                picker.connection = lux::editor::detail::takeConnection(
                    connect(
                        element,
                        &AssetPickerElement::edited,
                        [this, index, element](lux::ui::EditResult change) noexcept {
                            if (!change.changed || index >= textures_.values.size())
                                return;
                            textures_.values[index].texture = element->value();
                            textures_.changed = true;
                        }
                    ),
                    status
                );
                if (!status)
                    return;
                texture_pickers_.push_back(std::move(picker));
            }
        }
        Impl& editor_;

        template <class Result> bool accept(const Result& result)
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
                error_ =
                    result.error().domain + ":" + std::to_string(result.error().reason) + " " + result.error().message;
            }
            return false;
        }
        bool unchanged(editing::StateId state)
        {
            const auto history_ = editor_.historyView();
            if (!history_ || history_->history.current != state)
            {
                error_ = "The material changed during this gesture. Start the edit again.";
                return false;
            }
            return true;
        }
        lux::ui::SizeHint sizeHintContent() noexcept override
        {
            return {{320, 240}, {640, 480}};
        }
        void draw() noexcept override
        {

            if (!canvas_)
            {
                ImGui::TextUnformatted("Node canvas could not be created");
                return;
            }
            CanvasScope scope(canvas_.get());
            const auto history_ = editor_.historyView();
            if (!history_)
            {
                accept(history_);
                return;
            }
            ImGui::BeginDisabled(!editor_.project().writable());
            if (source_changed_ && !name_active_)
            {
                name_ = editor_.source().name;
            }
            ImGui::SetNextItemWidth(240);
            ImGui::InputText("Name", &name_);
            if (ImGui::IsItemActivated())
            {
                name_base_ = history_->history.current;
                name_active_ = true;
            }
            if (ImGui::IsItemDeactivated())
            {
                if (ImGui::IsItemDeactivatedAfterEdit() && unchanged(name_base_))
                {
                    accept(editor_.rename(name_));
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
                        accept(editor_.insertNode(node));
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();

            drawSettings(history_->history.current);
            drawProperties(history_->history.current);
            if (!error_.empty())
            {
                {
                    const auto& message_value = error_;
                    const std::string_view message{message_value};
                    ImGui::TextUnformatted(
                        message.empty() ? "" : message.data(),
                        message.empty() ? "" : message.data() + message.size()
                    );
                }
            }
            source_changed_ = false;
            drawCanvas();
        }
        void drawCanvas()
        {
            const auto& graph = editor_.source().graph;
            const bool writable = editor_.project().writable();
            canvas::Begin("Material graph");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                move_base_ = editor_.historyView()->history.current;
            }
            std::size_t ordinal{};
            for (const auto& record : graph.topology().nodes())
            {
                const auto* node = graph.node(record.id);
                const auto* author = graph.layout().find(record.id);
                const auto layout = author ? *author : lux::graph::GraphNodeLayout{};
                auto [position, inserted] = positions_.try_emplace(record.id);
                if (inserted || position->second.author != layout)
                {
                    position->second.author = layout;
                    position->second.display =
                        layout.placed
                            ? layout
                            : lux::graph::GraphNodeLayout{float(ordinal % 4) * 240, float(ordinal / 4) * 280, true};
                    canvas::SetNodePosition(
                        canvas_ids_.node(record.id.value),
                        {position->second.display.x, position->second.display.y}
                    );
                }
                ++ordinal;
                canvas::BeginNode(canvas_ids_.node(record.id.value));
                ImGui::TextUnformatted(
                    node->name().empty() ? lux::material::toString(node->kind()) : node->name().c_str()
                );
                for (const auto& pin : node->inputs())
                {
                    canvas::BeginPin(canvas_ids_.pin(pin.id.value), canvas::PinKind::Input);
                    ImGui::Text("< %s", pin.name.c_str());
                    canvas::EndPin();
                }
                if (const auto* constant = node->as<lux::material::ConstantNode>())
                {
                    std::array<float, 4> value;
                    std::ranges::copy(constant->value, value.begin());
                    if (auto* gesture = std::get_if<ConstantGesture>(&constant_); gesture && gesture->node == record.id)
                    {
                        value = gesture->value;
                    }
                    ImGui::PushID(static_cast<int>(record.id.value));
                    ImGui::BeginDisabled(!writable);
                    ImGui::SetNextItemWidth(220);
                    ImGui::DragFloat4("##value", value.data(), 0.01F);
                    if (ImGui::IsItemActivated())
                    {
                        constant_.emplace<ConstantGesture>(record.id, editor_.historyView()->history.current, value);
                    }
                    if (auto* gesture = std::get_if<ConstantGesture>(&constant_); gesture && gesture->node == record.id)
                    {
                        gesture->value = value;
                        if (ImGui::IsItemDeactivated())
                        {
                            if (ImGui::IsItemDeactivatedAfterEdit() && unchanged(gesture->base))
                            {
                                accept(editor_.setConstant(record.id, gesture->value));
                            }
                            constant_.emplace<Idle>();
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                for (const auto& pin : node->outputs())
                {
                    canvas::BeginPin(canvas_ids_.pin(pin.id.value), canvas::PinKind::Output);
                    ImGui::Text("%s >", pin.name.c_str());
                    canvas::EndPin();
                }
                canvas::EndNode();
            }
            const auto links = graph.topology().links();
            for (const auto& link : links)
            {
                const auto key = std::pair{link.from.value, link.to.value};
                auto [found, inserted] = links_.try_emplace(key, next_link_);
                if (inserted)
                {
                    ++next_link_;
                }
                canvas::Link(
                    canvas_ids_.link(found->second),
                    canvas_ids_.pin(link.from.value),
                    canvas_ids_.pin(link.to.value)
                );
            }
            lux::graph::LinkRecord created;
            if (writable && canvas::BeginCreate())
            {
                canvas::PinId first, second;
                if (canvas::QueryNewLink(&first, &second) && first && second && canvas::AcceptNewItem())
                {
                    created = {{canvas_ids_.source(first)}, {canvas_ids_.source(second)}};
                    const auto* pin = graph.topology().findPin(created.from);
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
                        removed_nodes.push_back(lux::material::NodeId{canvas_ids_.source(node)});
                    }
                }
                canvas::LinkId link;
                while (canvas::QueryDeletedLink(&link))
                {
                    const auto found = std::ranges::find_if(links_, [&](const auto& value) {
                        return value.second == canvas_ids_.source(link);
                    });
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
                accept(editor_.removeNodes(removed_nodes, removed_links));
            }
            if (created.from.valid() && created.to.valid())
            {
                accept(editor_.connect(created.from, created.to));
            }
            if (writable && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                std::vector<lux::graph::GraphLayoutEntry> moved;
                for (auto& [node, position] : positions_)
                {
                    if (!graph.node(node))
                    {
                        continue;
                    }
                    const auto value = canvas::GetNodePosition(canvas_ids_.node(node.value));
                    if (value.x != position.display.x || value.y != position.display.y)
                    {
                        moved.push_back({node, {value.x, value.y, true}});
                    }
                }
                if (!moved.empty() && (!unchanged(move_base_) || !accept(editor_.moveNodes(moved))))
                {
                    for (const auto& item : moved)
                    {
                        const auto& position = positions_.at(item.node).display;
                        canvas::SetNodePosition(canvas_ids_.node(item.node.value), {position.x, position.y});
                    }
                }
            }
            std::erase_if(positions_, [&](const auto& item) { return !graph.node(item.first); });
            std::erase_if(links_, [&](const auto& item) {
                return !graph.topology().findLink(
                    lux::graph::PinId{item.first.first},
                    lux::graph::PinId{item.first.second}
                );
            });
        }

        static bool valueType(const char* label, lux::material::EValueType& value)
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

        bool drawPayload(lux::material::Node& node)
        {
            using namespace lux::material;
            bool changed{};
            if (auto* constant = node.as<ConstantNode>())
            {
                auto type = constant->value_type;
                if (valueType("Type", type))
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
                changed |= slotChoice("Texture slot", sample->texture_slot, editor_.source().graph.texture_slots);
            }
            else if (auto* parameter = node.as<ParamNode>())
            {
                const auto& slots = editor_.source().graph.param_slots;
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
                if (valueType("Operand type", type))
                {
                    math->setOperandType(type);
                    changed = true;
                }
            }
            else if (auto* swizzle = node.as<SwizzleNode>())
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
            else if (auto* construct = node.as<ConstructNode>())
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

        template <class Slot> void drawSlots(TSlotDraft<Slot>& draft, editing::StateId current)
        {
            using namespace lux::material;
            constexpr bool textures = std::same_as<Slot, TextureSlotDecl>;
            const auto& values = [&]() -> const std::vector<Slot>& {
                if constexpr (textures)
                {
                    return editor_.source().graph.texture_slots;
                }
                else
                {
                    return editor_.source().graph.param_slots;
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
                auto& slot = draft.values[index];
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::TreeNodeEx("slot", ImGuiTreeNodeFlags_DefaultOpen, "Slot %zu", index))
                {
                    draft.changed |= ImGui::InputText("Name", &slot.name);
                    if constexpr (textures)
                    {
                        if (index < texture_pickers_.size())
                        {
                            auto& picker = *texture_pickers_[index].element;
                            picker.setValue(slot.texture);
                            const auto cursor = ImGui::GetCursorScreenPos();
                            const auto hint = picker.measure(ImGui::GetContentRegionAvail().x);
                            picker.arrange(
                                {{cursor.x - contentOrigin().x, cursor.y - contentOrigin().y},
                                 {ImGui::GetContentRegionAvail().x, hint.preferred.height}}
                            );
                            drawChild(picker);
                            ImGui::SetCursorScreenPos(cursor);
                            ImGui::Dummy({hint.preferred.width, hint.preferred.height});
                        }
                    }
                    else
                    {
                        draft.changed |= valueType("Type", slot.type);
                        draft.changed |= ImGui::DragScalarN(
                            "Default",
                            ImGuiDataType_Float,
                            slot.dflt,
                            static_cast<int>(slot.type) + 1,
                            0.01F
                        );
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::BeginDisabled(draft.values.size() >= MaterialSourceLimits{}.max_slots);
            if (ImGui::SmallButton("Add slot"))
            {
                auto& slot = draft.values.emplace_back();
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
                        return editor_.setTextureSlots(draft.base, draft.values);
                    }
                    else
                    {
                        return editor_.setParameterSlots(draft.base, draft.values);
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
            ImGui::BeginDisabled(!editor_.project().writable());
            if (!state_changed_ && state_base_ != current)
            {
                render_state_ = editor_.source().graph.render_state;
                state_base_ = current;
            }
            int shading_model = static_cast<int>(editor_.source().graph.shading_model);
            if (ImGui::Combo(
                    "Shading model",
                    &shading_model,
                    "Unlit\0Legacy lit\0PBR metallic / roughness\0Stylized\0Graph\0"
                ))
            {
                accept(editor_.setShadingModel(static_cast<lux::rdesc::ELightingTechnique>(shading_model)));
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
                if (accept(editor_.setRenderState(render_state_)))
                {
                    state_changed_ = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert render state"))
            {
                render_state_ = editor_.source().graph.render_state;
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
            const auto* node = editor_.source().graph.node(lux::material::NodeId{canvas_ids_.source(selected.front())});
            if (!node)
            {
                draft_.emplace<Idle>();
                return;
            }
            auto* draft = std::get_if<NodeDraft>(&draft_);
            if (!draft || draft->value->id() != node->id() || (!draft->changed && draft->base != current))
            {
                draft = &draft_.emplace<NodeDraft>(current, node->clone(), node->name(), false);
            }
            ImGui::BeginChild("node-properties", {0, 230}, ImGuiChildFlags_Borders);
            ImGui::BeginDisabled(!editor_.project().writable());
            if (ImGui::InputText("Node name", &draft->name))
            {
                draft->value->setName(draft->name);
                draft->changed = true;
            }
            draft->changed |= drawPayload(*draft->value);
            const bool apply = ImGui::Button("Apply properties");
            ImGui::SameLine();
            const bool revert = ImGui::Button("Revert properties");
            if (apply && accept(editor_.replaceNode(draft->base, draft->value)))
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
        NodeCanvasIds canvas_ids_;
        std::unique_ptr<canvas::EditorContext, CanvasDelete> canvas_;
        std::unordered_map<lux::material::NodeId, Position> positions_;
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> links_;
        std::uint64_t next_link_{1};
        std::variant<Idle, ConstantGesture> constant_;
        std::variant<Idle, NodeDraft> draft_;
        TSlotDraft<lux::material::TextureSlotDecl> textures_;
        TSlotDraft<lux::material::ParamSlotDecl> parameters_;
        lux::material::RenderState render_state_;
        editing::StateId state_base_;
        bool state_changed_{};
        std::string name_, error_;
        editing::StateId name_base_, move_base_;
        bool source_changed_{true}, name_active_{};
        editing::Revision observed_revision_;
        editing::HistoryId observed_history_;
        std::vector<TexturePicker> texture_pickers_;
        object::Connection content_connection_;
    };

    class MaterialEditor::Impl::Content final : public lux::ui::Element
    {
    public:
        explicit Content(
            Impl& editor_,
            EditorResult<void>& status,
            lux::scene::SceneRuntime& runtime,
            lux::scene::RenderResources& resources,
            lux::system::SystemInstanceId render_system
        )
            : lux::ui::Element(*editor_.editor_, lux::ui::ElementId{"content"}), editor_(editor_),
              layout_(*this, lux::ui::ElementId{"layout"}),
              asset_actions_(layout_, *editor_.editor_, editor_.project(), status),
              work_(layout_, lux::ui::ElementId{"work"}, lux::ui::ELayoutType::HORIZONTAL),
              graph_(work_, editor_, status), side_(work_, lux::ui::ElementId{"compilation"}),
              actions_(side_, lux::ui::ElementId{"actions"}, lux::ui::ELayoutType::HORIZONTAL),
              compile_button_(actions_, lux::ui::ElementId{"compile"}, "Compile"),
              publish_button_(actions_, lux::ui::ElementId{"publish"}, "Save source and publish asset"),
              status_(side_, lux::ui::ElementId{"status"}, "Not compiled"),
              publication_(side_, lux::ui::ElementId{"publication"}, *editor_.editor_, status),
              preview_(side_, editor_, runtime, resources, render_system),
              compile_connection_(lux::editor::detail::takeConnection(
                  lux::object::LuxObject::connect(
                      std::addressof(compile_button_),
                      &lux::ui::Button::activated,
                      [this]() noexcept { action_ = EAction::COMPILE; }
                  ),
                  status
              )),
              publish_connection_(lux::editor::detail::takeConnection(
                  lux::object::LuxObject::connect(
                      std::addressof(publish_button_),
                      &lux::ui::Button::activated,
                      [this]() noexcept { action_ = EAction::PUBLISH; }
                  ),
                  status
              ))
        {
            graph_.setStretch({2, 1});
            actions_.setStretch({1, 0});
            publish_button_.setEnabled(false);
        }
        EditorResult<void> finishEditing()
        {
            return graph_.finishEditing();
        }
        void command(object::EventView& event) noexcept
        {
            asset_actions_.command(event);
        }
        void applyControls() noexcept
        {
            if (action_ == EAction::NONE)
                return;
            const auto finished = editor_.finishEditing();
            if (!finished)
            {
                status_.setText(finished.error().message);
                return;
            }
            if (action_ == EAction::COMPILE)
            {
                const auto request = editor_.requestCompile();
                if (request)
                {
                    compile_ = *request;
                    error_.clear();
                }
                else
                    error_ = request.error().message;
            }
            else
            {
                const auto request = editor_.requestPublish(compile_, "desktop");
                if (request)
                {
                    publication_.track(*request);
                    error_.clear();
                }
                else
                    error_ = request.error().message;
            }
            action_ = EAction::NONE;
        }

    private:
        lux::ui::SizeHint sizeHintContent() noexcept override
        {
            return layout_.sizeHint();
        }
        lux::ui::SizeHint measureContent(float width) noexcept override
        {
            return layout_.measure(width);
        }
        void arrangeContent() noexcept override
        {
            layout_.arrange({{}, rect().size});
        }
        void draw() noexcept override
        {
            drawChild(layout_);
        }
        void update() noexcept override
        {
            const bool available = !editor_.source_.id.isNull() && editor_.asset_status_.phase == EAssetEditPhase::IDLE;
            graph_.setEnabled(available);
            compile_button_.setEnabled(available);
            publish_button_.setEnabled(false);
            if (!error_.empty())
            {
                status_.setText(error_);
                return;
            }
            if (!static_cast<bool>(compile_))
                return;
            const auto state = editor_.compileStatus(compile_);
            if (!state)
            {
                if (state.error().code == EEditorError::STALE_REQUEST)
                {
                    compile_ = {};
                    error_.clear();
                    status_.setText("Not compiled");
                }
                else
                    status_.setText(state.error().message);
                return;
            }
            compile_button_.setEnabled(!std::holds_alternative<material::MaterialCompilePending>(*state));
            if (const auto* success = std::get_if<material::MaterialCompileSucceeded>(&*state))
            {
                status_.setText(
                    success->current ? "Compilation succeeded" : "Changed; compile again to update the result"
                );
                publish_button_.setEnabled(
                    success->current && editor_.project().writable() && editor_.saveRequests().empty()
                );
            }
            else if (const auto* failure = std::get_if<material::MaterialCompileFailed>(&*state))
                status_.setText(failure->failure.message);
            else
                status_.setText("Compiling...");
        }
        Impl& editor_;
        enum class EAction : std::uint8_t
        {
            NONE,
            COMPILE,
            PUBLISH
        };
        EAction action_{};
        lux::process::TaskId compile_;
        std::string error_;
        lux::ui::Layout layout_;
        TAssetActions<material::MaterialEditor> asset_actions_;
        lux::ui::Layout work_;
        GraphElement graph_;
        lux::ui::Layout side_, actions_;
        lux::ui::Button compile_button_, publish_button_;
        lux::ui::Label status_;
        TPublicationControls<material::MaterialEditor> publication_;
        MaterialPreviewElement preview_;
        object::Connection compile_connection_, publish_connection_;
    };
} // namespace lux::editor::material

namespace lux::editor::material
{
    void MaterialEditor::Impl::createContent(EditorResult<void>& status)
    {
        content_ = std::make_unique<Content>(
            *this,
            status,
            editor_context_.engine().sceneRuntime(),
            editor_context_.renderResources(),
            preview_render_system_
        );
        if (!status)
            return;
        editor_->setContent(*content_);
        editor_->root().setDockLayout(
            {{},
             std::string(editor_->id().name()),
             {},
             {},
             0,
             0,
             0,
             editor_->root().findPane(lux::ui::PaneIdView{"project"}) ? "project" : ""}
        );
    }
    EditorResult<void> MaterialEditor::Impl::finishContentEditing()
    {
        return content_ ? content_->finishEditing() : EditorResult<void>{};
    }
    void MaterialEditor::Impl::contentCommand(object::EventView& event) noexcept
    {
        if (content_)
            content_->command(event);
    }
    void MaterialEditor::Impl::applyContentIntents() noexcept
    {
        if (content_)
            content_->applyControls();
    }
}

namespace lux::editor::material
{
    MaterialEditor::Impl::Impl(EditorContext& context)
        : editor_context_(context), completion_work_(context.execution(), this, [](void* owner) noexcept {
              static_cast<Impl*>(owner)->completion_deferred_ = true;
          })
    {}
    MaterialEditor::Impl::~Impl()
    {
        reading_ = {};
        compilation_.reset();
        completion_work_.cancel();
    }
}
