#include <lux/engine/editor/material/PublishCompiledMaterial.hpp>
#include <lux/engine/editor/views/ViewportElement.hpp>
#include <lux/engine/editor/detail/ViewportStateCodec.hpp>
#include <lux/engine/editor/workbench/InteractionDelivery.hpp>
#include <lux/engine/editor/material/MaterialView.hpp>
#include <lux/engine/editor/workbench/ViewPreparation.hpp>
#include <lux/engine/editor/material/MaterialNodeControls.hpp>
#include <lux/engine/editor/project/AssetPickerElement.hpp>
#include <lux/engine/editor/widgets/GraphCanvas.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <deque>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace lux::editor::material
{
    namespace
    {
        template <class T> auto rejected(T error)
        {
            return cxx::unexpected(VMaterialViewFailure{std::move(error)});
        }
        template <class T> MaterialViewResult<void> accepted(T result)
        {
            if (!result)
                return rejected(result.error());
            return {};
        }
        bool temporary(const VMaterialViewFailure& failure)
        {
            return std::visit(
                [](const auto& error) {
                    using T = std::decay_t<decltype(error)>;
                    if constexpr (std::same_as<T, MaterialEditError>)
                        return error.code == EMaterialEditError::SESSION &&
                               error.session == sessions::ESessionError::BUSY;
                    else if constexpr (std::same_as<T, views::EViewError>)
                        return error == views::EViewError::BUSY;
                    else if constexpr (std::same_as<T, VMaterialCompileFailure>)
                    {
                        const auto* code = std::get_if<EMaterialCompileRequestError>(&error);
                        return code && *code == EMaterialCompileRequestError::BUSY;
                    }
                    else if constexpr (std::same_as<T, MaterialPreviewFailure>)
                        return error.code == EMaterialPreviewError::BUSY;
                    else
                        return false;
                },
                failure
            );
        }
        struct Display final
        {
            sessions::ContentStamp content;
            std::string name;
            std::vector<widgets::CanvasNode> nodes;
            std::vector<widgets::CanvasLink> links;
            std::vector<lux::material::TextureSlotDecl> textures;
            std::vector<lux::material::ParamSlotDecl> parameters;
            lux::material::RenderState render;
            lux::rdesc::ELightingTechnique shading;
        };
        MaterialViewResult<Display> display(const MaterialSession& session)
        {
            const auto stamp = session.describe().current;
            auto read = session.read();
            if (!read)
                return rejected(read.error());
            auto values =
                read->withRead([&](const lux::material::MaterialSource& source) -> MaterialEditResult<Display> {
                    Display result;
                    result.content = stamp;
                    result.name = source.name;
                    result.textures = source.graph.texture_slots;
                    result.parameters = source.graph.param_slots;
                    result.render = source.graph.render_state;
                    result.shading = source.graph.shading_model;
                    for (const auto& record : source.graph.topology().nodes())
                    {
                        const auto* node = source.graph.node(record.id);
                        widgets::CanvasNode row{record.id.value, node->name()};
                        for (const auto& pin : node->inputs())
                            row.pins.push_back({pin.id.value, pin.name, true});
                        for (const auto& pin : node->outputs())
                            row.pins.push_back({pin.id.value, pin.name, false});
                        if (const auto* position = source.graph.layout().find(record.id))
                        {
                            row.position = {position->x, position->y};
                            row.placed = position->placed;
                        }
                        result.nodes.push_back(std::move(row));
                    }
                    for (const auto& link : source.graph.topology().links())
                        result.links.push_back({link.from.value, link.to.value});
                    return result;
                });
            if (!values)
                return rejected(values.error());
            return std::move(*values);
        }
    }
    struct MaterialView::Impl final
    {
        enum class EControl : std::uint8_t
        {
            NONE,
            UNDO,
            REDO,
            COMPILE,
            PUBLISH,
            CANCEL,
            APPLY_NODE,
            REVERT_NODE
        };
        MaterialView& view_;
        std::unique_ptr<MaterialPreview> preview_owner_;
        std::unique_ptr<MaterialInteraction> interaction_;
        MaterialViewServices services_;
        MaterialCompileId compile_;
        lux::scene::RenderAssetInput compile_assets_;
        MaterialCompileId delivered_;
        std::optional<MaterialViewBinding> binding_;
        MaterialViewState state_;
        Display display_;
        MaterialViewResult<void> status_;
        struct NodePropertiesDraft final
        {
            sessions::ContentStamp based_on;
            MaterialReplaceNode value;
        };
        std::optional<NodePropertiesDraft> draft_;
        lux::material::NodeId selected_node_;
        std::optional<std::uint64_t> selection_request_;
        using ECanvasStage = workbench::detail::EInputDeliveryStage;
        struct CanvasRequest final
        {
            sessions::ContentStamp based_on;
            widgets::CanvasEdit input;
            std::vector<VMaterialEdit> edits;
            ECanvasStage stage;
        };
        std::deque<CanvasRequest> canvas_request_;
        EControl control_{};
        lux::scene::SceneInstanceId presented_;
        lux::ui::Layout layout_, side_;
        widgets::GraphCanvas graph_;
        lux::editor::views::ViewportElement viewport_;
        template <class MakeEdit> MaterialViewResult<void> enqueue(sessions::ContentStamp based_on, MakeEdit make_edit)
        {
            if (canvas_request_.size() >= 64)
                return rejected(views::EViewError::CAPACITY);
            canvas_request_.push_back({based_on, widgets::CanvasEdit{{}, true, true, false}, {}, ECanvasStage::BEGIN});
            canvas_request_.back().edits.emplace_back(make_edit());
            return {};
        }
        MaterialViewResult<void> validate(sessions::ContentStamp based_on) const
        {
            auto info = services_.sessions.describe(binding_->session);
            if (!info)
                return rejected(MaterialEditError{info.error()});
            if (info->admission != sessions::EEditAdmission::AVAILABLE)
                return rejected(MaterialEditError{sessions::ESessionError::BUSY});
            if (info->current != based_on)
                return rejected(MaterialEditError{EMaterialEditError::STALE_CONTENT});
            return {};
        }
        MaterialViewResult<void> rejectInput(const VMaterialViewFailure& failure)
        {
            if (temporary(failure))
                return cxx::unexpected(failure);
            const auto based_on = canvas_request_.front().based_on;
            const auto* overlay = binding_->interaction->overlay();
            if (overlay && overlay->expected == based_on)
            {
                auto cancelled = binding_->interaction->cancel();
                if (!cancelled)
                    return rejected(cancelled.error());
            }
            const auto clear = [&] {
                canvas_request_.pop_front();
                graph_.setEnabled(canvas_request_.size() < 60);
            };
            auto owner = services_.sessions.read(binding_->session);
            if (!owner)
            {
                if (owner.error() != sessions::ESessionError::STALE_SESSION)
                    return rejected(MaterialEditError{owner.error()});
                clear();
                return cxx::unexpected(failure);
            }
            auto read = owner->get().read();
            if (!read)
                return rejected(read.error());
            auto cleared = read->withRead([&](const lux::material::MaterialSource&) -> MaterialEditResult<void> {
                clear(); // A terminal input cannot block later requests; payload destruction keeps the original gate.
                return {};
            });
            if (!cleared)
                return rejected(cleared.error());
            return cxx::unexpected(failure);
        }
        struct Properties final : lux::ui::Element
        {
            Impl& state_;
            struct Picker final
            {
                std::unique_ptr<project::AssetPickerElement> element;
                object::Connection connection;
            };
            std::vector<Picker> pickers_;
            std::string name_;
            explicit Properties(lux::ui::Element& parent, Impl& state)
                : Element(parent, lux::ui::ElementId{"properties"}), state_(state)
            {
                setStretch({1, 1});
            }
            void synchronize()
            {
                pickers_.resize(std::min(pickers_.size(), state_.display_.textures.size()));
                while (pickers_.size() < state_.display_.textures.size())
                {
                    const auto index = pickers_.size();
                    auto picker = std::make_unique<project::AssetPickerElement>(
                        *this,
                        lux::ui::ElementId{"texture-" + std::to_string(index)},
                        state_.services_.assets,
                        asset::TextureAsset::primary_magic,
                        state_.display_.textures[index].texture
                    );
                    auto* value = picker.get();
                    auto connected = connect(
                        value,
                        &project::AssetPickerElement::edited,
                        [this, index, value](lux::ui::EditResult edit) noexcept {
                            if (edit.changed && index < state_.display_.textures.size())
                                state_.display_.textures[index].texture = value->value();
                        }
                    );
                    if (!connected)
                    {
                        state_.status_ = rejected(views::EViewError::CAPACITY);
                        return;
                    }
                    pickers_.push_back({std::move(picker), std::move(*connected)});
                }
                for (std::size_t i{}; i < pickers_.size(); ++i)
                    pickers_[i].element->setValue(state_.display_.textures[i].texture);
                name_ = state_.display_.name;
            }
            void draw() noexcept override
            {
                ImGui::BeginDisabled(!state_.binding_);
                if (ImGui::Button("Undo"))
                    state_.control_ = EControl::UNDO;
                ImGui::SameLine();
                if (ImGui::Button("Redo"))
                    state_.control_ = EControl::REDO;
                ImGui::BeginDisabled(!state_.binding_);
                if (ImGui::Button("Compile"))
                    state_.control_ = EControl::COMPILE;
                ImGui::SameLine();
                if (ImGui::Button("Publish compiled"))
                    state_.control_ = EControl::PUBLISH;
                ImGui::EndDisabled();
                if (!state_.status_)
                    ImGui::TextUnformatted("Input rejected; Revert node or Cancel pending edits to recover.");
                if (ImGui::Button("Cancel pending edits"))
                    state_.control_ = EControl::CANCEL;
                const auto preview = state_.services_.preview.status();
                if (preview.stale)
                    ImGui::TextUnformatted("Preview is stale; compile the current source.");
                if (!preview.diagnostic.empty())
                    ImGui::TextWrapped("%s", preview.diagnostic.c_str());
                if (ImGui::InputText("Name", &name_, ImGuiInputTextFlags_EnterReturnsTrue))
                    state_.status_ = state_.enqueue(state_.display_.content, [&] { return MaterialRename{name_}; });
                if (ImGui::BeginCombo("Add node", "Choose node kind"))
                {
                    for (auto kind = 1; kind < static_cast<int>(lux::material::EMatNodeKind::COUNT); ++kind)
                    {
                        const auto type = static_cast<lux::material::EMatNodeKind>(kind);
                        if (ImGui::Selectable(lux::material::toString(type)))
                            state_.status_ = state_.enqueue(state_.display_.content, [&] {
                                return MaterialInsertNode{contracts::CodeLease::builtin(), makeMaterialNode(type)};
                            });
                    }
                    ImGui::EndCombo();
                }
                if (state_.draft_ && ImGui::CollapsingHeader("Node properties", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    auto session = state_.services_.sessions.read(state_.binding_->session);
                    if (session)
                    {
                        auto read = session->get().read();
                        if (read)
                        {
                            auto rendered =
                                read->withRead([&](const lux::material::MaterialSource&) -> MaterialEditResult<void> {
                                    static_cast<void>(editMaterialNodePayload(
                                        *state_.draft_->value.value,
                                        state_.display_.textures,
                                        state_.display_.parameters
                                    ));
                                    return {};
                                });
                            if (!rendered)
                                state_.status_ = rejected(rendered.error());
                        }
                        else
                            state_.status_ = rejected(read.error());
                    }
                    else
                        state_.status_ = rejected(MaterialEditError{session.error()});
                    if (ImGui::Button("Apply node"))
                        state_.control_ = EControl::APPLY_NODE;
                    ImGui::SameLine();
                    if (ImGui::Button("Revert node"))
                        state_.control_ = EControl::REVERT_NODE;
                }
                if (ImGui::CollapsingHeader("Material settings"))
                {
                    auto& data = state_.display_;
                    int shading = static_cast<int>(data.shading);
                    if (ImGui::Combo(
                            "Shading",
                            &shading,
                            "Unlit\0Legacy lit\0PBR metallic / roughness\0Stylized\0Graph\0"
                        ))
                        state_.status_ = state_.enqueue(state_.display_.content, [&] {
                            return MaterialSetShading{static_cast<lux::rdesc::ELightingTechnique>(shading)};
                        });
                    int alpha = static_cast<int>(data.render.alpha_mode);
                    if (ImGui::Combo("Alpha", &alpha, "Opaque\0Mask\0Blend\0"))
                        data.render.alpha_mode = static_cast<lux::rdesc::EAlphaMode>(alpha);
                    ImGui::SliderFloat("Cutoff", &data.render.alpha_cutoff, 0, 1);
                    ImGui::Checkbox("Double sided", &data.render.double_sided);
                    if (ImGui::Button("Apply render state"))
                        state_.status_ = state_.enqueue(state_.display_.content, [&] {
                            return MaterialSetRenderState{data.render};
                        });
                    if (ImGui::TreeNode("Textures"))
                    {
                        for (std::size_t i{}; i < data.textures.size(); ++i)
                        {
                            ImGui::PushID(static_cast<int>(i));
                            ImGui::InputText("Name", &data.textures[i].name);
                            if (i < pickers_.size())
                            {
                                auto& picker = *pickers_[i].element;
                                const auto cursor = ImGui::GetCursorScreenPos();
                                const auto size = picker.measure(ImGui::GetContentRegionAvail().x).preferred;
                                picker.arrange({{cursor.x - contentOrigin().x, cursor.y - contentOrigin().y}, size});
                                drawChild(picker);
                                ImGui::SetCursorScreenPos(cursor);
                                ImGui::Dummy({size.width, size.height});
                            }
                            ImGui::PopID();
                        }
                        if (ImGui::SmallButton("Add texture"))
                            state_.status_ = state_.enqueue(state_.display_.content, [&] {
                                return MaterialSetTextureSlots{[&] {
                                    auto values = data.textures;
                                    values.push_back({"Texture", {}});
                                    return values;
                                }()};
                            });
                        if (ImGui::Button("Apply textures"))
                            state_.status_ = state_.enqueue(state_.display_.content, [&] {
                                return MaterialSetTextureSlots{data.textures};
                            });
                        ImGui::TreePop();
                    }
                    if (ImGui::TreeNode("Parameters"))
                    {
                        for (std::size_t i{}; i < data.parameters.size(); ++i)
                        {
                            auto& parameter = data.parameters[i];
                            ImGui::PushID(static_cast<int>(i));
                            ImGui::InputText("Name", &parameter.name);
                            static_cast<void>(editMaterialValueType("Type", parameter.type));
                            ImGui::DragScalarN(
                                "Default",
                                ImGuiDataType_Float,
                                parameter.dflt,
                                static_cast<int>(parameter.type) + 1,
                                .01F
                            );
                            ImGui::PopID();
                        }
                        if (ImGui::SmallButton("Add parameter"))
                            state_.status_ = state_.enqueue(state_.display_.content, [&] {
                                return MaterialSetParameterSlots{[&] {
                                    auto values = data.parameters;
                                    values.push_back({"Parameter"});
                                    return values;
                                }()};
                            });
                        if (ImGui::Button("Apply parameters"))
                            state_.status_ = state_.enqueue(state_.display_.content, [&] {
                                return MaterialSetParameterSlots{data.parameters};
                            });
                        ImGui::TreePop();
                    }
                }
                ImGui::EndDisabled();
            }
        } properties_;
        std::array<object::Connection, 3> connections_;
        lux::editor::views::CameraMotion motion_;
        bool motion_pending_{}, camera_pending_{};
        Impl(MaterialView& view, MaterialViewServices services, MaterialViewState state)
            : view_(view), services_(services), state_(state),
              layout_(view, lux::ui::ElementId{"content"}, lux::ui::ELayoutType::HORIZONTAL),
              side_(layout_, lux::ui::ElementId{"side"}), graph_(layout_, lux::ui::ElementId{"graph"}),
              viewport_(side_, lux::ui::ElementId{"preview"}), properties_(side_, *this)
        {
            side_.setStretch({1, 1});
            graph_.setStretch({2, 1});
            view.setContent(layout_);
            auto changed = object::LuxObject::connect(
                &graph_,
                &widgets::GraphCanvas::edited,
                [this](const widgets::CanvasEdit& edit) noexcept {
                    if (canvas_request_.size() >= 64)
                    {
                        status_ = rejected(views::EViewError::CAPACITY);
                        return;
                    }
                    const auto stage = edit.cancelled ? ECanvasStage::CANCEL
                                       : edit.began   ? ECanvasStage::BEGIN
                                                      : ECanvasStage::PREVIEW;
                    canvas_request_.push_back({display_.content, edit, {}, stage});
                    graph_.setEnabled(canvas_request_.size() < 60);
                }
            );
            auto selected = object::LuxObject::connect(
                &graph_,
                &widgets::GraphCanvas::selected,
                [this](std::span<const std::uint64_t> ids) noexcept {
                    selection_request_ = ids.size() == 1 ? ids.front() : 0;
                }
            );
            viewport_.enableNavigation(true);
            auto navigation = object::LuxObject::connect(
                &viewport_,
                &lux::editor::views::ViewportElement::cameraMoved,
                [this](const lux::editor::views::CameraMotion& motion) noexcept {
                    motion_.angular_delta += motion.angular_delta;
                    motion_.pan_delta += motion.pan_delta;
                    motion_.dolly += motion.dolly;
                    motion_pending_ = true;
                }
            );
            if (navigation)
                connections_[2] = std::move(*navigation);
            if (!changed || !selected || !navigation)
                status_ = rejected(views::EViewError::CAPACITY);
            else
            {
                connections_[0] = std::move(*changed);
                connections_[1] = std::move(*selected);
            }
        }
        ~Impl() noexcept
        {
            if (!services_.compilation.releaseResult(compile_))
                std::terminate(); // The view and its service share the owner thread.
            if (!discardInputs())
                std::terminate();
        }
        MaterialViewResult<void> install(Display candidate)
        {
            if (!graph_.setGraph(std::move(candidate.nodes), std::move(candidate.links), canvas_request_.empty()))
                return rejected(views::EViewError::BUSY);
            display_ = std::move(candidate);
            properties_.synchronize();
            return {};
        }
        MaterialViewResult<void> discardInputs()
        {
            const auto clear = [&] {
                draft_.reset();
                selection_request_.reset();
                selected_node_ = {};
                canvas_request_.clear();
                control_ = EControl::NONE;
                graph_.setEnabled(true);
            };
            if (!binding_)
            {
                clear();
                return {};
            }
            auto owner = services_.sessions.read(binding_->session);
            if (!owner)
            {
                if (owner.error() != sessions::ESessionError::STALE_SESSION)
                    return rejected(MaterialEditError{owner.error()});
                clear();
                return {};
            }
            auto read = owner->get().read();
            if (!read)
                return rejected(read.error());
            return accepted(read->withRead([&](const lux::material::MaterialSource&) -> MaterialEditResult<void> {
                clear();
                return {};
            }));
        }
        MaterialViewResult<void> rebind(std::optional<MaterialViewBinding> binding)
        {
            if (!view_.isOnAffinityThread() || object::LuxObject::isDispatching())
                return rejected(views::EViewError::BUSY);
            if (binding == binding_)
                return {};
            Display candidate;
            if (binding)
            {
                if (!binding->interaction || binding->interaction->session() != binding->session)
                    return rejected(views::EViewError::INVALID_ID);
                auto session = services_.sessions.read(binding->session);
                if (!session)
                    return rejected(MaterialEditError{session.error()});
                auto read = display(session->get());
                if (!read)
                    return cxx::unexpected(read.error());
                candidate = std::move(*read);
            }
            if (binding_)
            {
                auto ended = binding_->interaction->cancel();
                if (!ended)
                    return rejected(ended.error());
            }
            auto discarded = discardInputs();
            if (!discarded)
                return discarded;
            auto installed = install(std::move(candidate));
            if (!installed)
                return installed;
            if (!services_.compilation.releaseResult(compile_))
                std::terminate();
            compile_ = {};
            binding_ = binding;
            viewport_.setPresentation({}, state_.extent);
            presented_ = {};
            return {};
        }
        MaterialViewResult<void> select(lux::material::NodeId node)
        {
            if (!binding_)
                return rejected(views::EViewError::INVALID_ID);
            auto session = services_.sessions.read(binding_->session);
            if (!session)
                return rejected(MaterialEditError{session.error()});
            auto read = session->get().read();
            if (!read)
                return rejected(read.error());
            const auto based_on = session->get().describe().current;
            std::optional<NodePropertiesDraft> candidate;
            if (node.valid())
            {
                auto copy = read->copyNode(node);
                if (!copy)
                    return rejected(copy.error());
                candidate.emplace(NodePropertiesDraft{based_on, std::move(*copy)});
            }
            // The synchronous operations below cannot release the live session: each callback uses
            // its existing admission. Any discarded clone (including the former draft after swap)
            // is destroyed under the same read gate, before its outer code lease.
            const auto dispose = [&] {
                auto cleared = read->withRead([&](const lux::material::MaterialSource&) -> MaterialEditResult<void> {
                    candidate.reset();
                    return {};
                });
                if (!cleared)
                    std::terminate();
            };
            auto ended = binding_->interaction->cancel();
            if (!ended)
            {
                dispose();
                return rejected(ended.error());
            }
            auto selection =
                binding_->interaction->select(node.valid() ? std::vector{node} : std::vector<lux::material::NodeId>{});
            if (!selection)
            {
                dispose();
                return rejected(selection.error());
            }
            auto adopted = read->withRead([&](const lux::material::MaterialSource&) -> MaterialEditResult<void> {
                canvas_request_.clear();
                draft_.swap(candidate);
                candidate.reset();
                selected_node_ = node;
                graph_.setEnabled(true);
                return {};
            });
            if (!adopted)
                dispose();
            return accepted(std::move(adopted));
        }
        MaterialViewResult<void> maintain()
        {
            if (!binding_)
                return {};
            if (control_ == EControl::CANCEL)
            {
                auto cancelled = view_.cancelEdit();
                if (!cancelled)
                    return cancelled;
                status_ = {};
            }
            if (control_ == EControl::REVERT_NODE)
            {
                auto reverted = select(selected_node_);
                if (reverted || !temporary(reverted.error()))
                    control_ = EControl::NONE;
                if (!reverted)
                    return reverted;
                status_ = {};
            }
            if (motion_pending_)
            {
                auto moved = view_.navigate(motion_);
                if (!moved)
                    return moved;
                motion_ = {};
                motion_pending_ = false;
            }
            if (camera_pending_ && viewport_.bound())
            {
                auto changed = viewport_.presentation().setCameraPose(state_.camera.transform, state_.camera.camera);
                if (!changed)
                    return rejected(changed.error());
                camera_pending_ = false;
            }
            auto sync = binding_->interaction->synchronize();
            if (!sync)
                return rejected(sync.error());
            if (selection_request_)
            {
                auto selected = select(lux::material::NodeId{*selection_request_});
                if (!selected)
                    return selected;
                status_ = {};
                selection_request_.reset();
            }
            while (!canvas_request_.empty())
            {
                auto& pending = canvas_request_.front();
                const auto& request = pending.input;
                auto delivered = workbench::detail::deliverInput(
                    pending.stage,
                    request.committed,
                    [&] { return validate(pending.based_on); },
                    [&] { return accepted(binding_->interaction->cancel()); },
                    [&] { return view_.beginEdit("Move/connect graph nodes"); },
                    [&]() -> MaterialViewResult<void> {
                        if (pending.edits.empty())
                        {
                            std::visit(
                                [&](const auto& value) {
                                    using T = std::decay_t<decltype(value)>;
                                    if constexpr (std::same_as<T, widgets::CanvasLink>)
                                        pending.edits.emplace_back(MaterialConnect{{value.from}, {value.to}});
                                    else if constexpr (std::same_as<T, widgets::CanvasErase>)
                                    {
                                        for (auto link : value.links)
                                            pending.edits.emplace_back(MaterialDisconnect{{link.from}, {link.to}});
                                        for (auto node : value.nodes)
                                            pending.edits.emplace_back(MaterialEraseNode{{node}});
                                    }
                                    else
                                        for (auto node : value.nodes)
                                            pending.edits.emplace_back(
                                                MaterialPlaceNode{{node.node}, {node.position.x, node.position.y, true}}
                                            );
                                },
                                request.value
                            );
                        }
                        return view_.previewEdit(pending.edits);
                    },
                    [&] {
                        auto committed = view_.commitEdit();
                        if (committed)
                            status_ = {};
                        return committed;
                    }
                );
                if (!delivered)
                    return pending.stage == ECanvasStage::CANCEL ? delivered : rejectInput(delivered.error());
                auto owner = services_.sessions.read(binding_->session);
                if (!owner)
                    return rejected(MaterialEditError{owner.error()});
                auto read = owner->get().read();
                if (!read)
                    return rejected(read.error());
                auto cleared = read->withRead([&](const lux::material::MaterialSource&) -> MaterialEditResult<void> {
                    canvas_request_.pop_front(); // Replaced preview payloads die under the author gate.
                    return {};
                });
                if (!cleared)
                    return rejected(cleared.error());
            }
            graph_.setEnabled(true);
            const auto command = control_;
            MaterialViewResult<void> command_result;
            switch (command)
            {
            case EControl::UNDO:
                command_result = view_.undo();
                break;
            case EControl::REDO:
                command_result = view_.redo();
                break;
            case EControl::COMPILE:
                command_result = accepted(view_.compile());
                break;
            case EControl::PUBLISH:
                command_result = accepted(view_.requestPublication());
                break;
            case EControl::CANCEL:
                command_result = view_.cancelEdit();
                break;
            case EControl::APPLY_NODE:
                if (draft_)
                {
                    command_result = validate(draft_->based_on);
                    if (!command_result)
                        break;
                    auto owner = services_.sessions.read(binding_->session);
                    if (!owner)
                        return rejected(MaterialEditError{owner.error()});
                    auto read = owner->get().read();
                    if (!read)
                        return rejected(read.error());
                    if (canvas_request_.size() >= 64)
                        return rejected(views::EViewError::CAPACITY);
                    command_result =
                        accepted(read->withRead([&](const lux::material::MaterialSource&) -> MaterialEditResult<void> {
                            canvas_request_.push_back(
                                {draft_->based_on, widgets::CanvasEdit{{}, true, true, false}, {}, ECanvasStage::BEGIN}
                            );
                            canvas_request_.back().edits.emplace_back(std::move(draft_->value));
                            draft_.reset();
                            return {};
                        }));
                }
                break;
            default:
                break;
            }
            if (command_result || !temporary(command_result.error()))
                control_ = EControl::NONE;
            if (!command_result)
                return command_result;
            auto owner = services_.sessions.read(binding_->session);
            if (!owner)
                return rejected(MaterialEditError{owner.error()});
            const auto info = owner->get().describe();
            if (!binding_->interaction->overlay() && display_.content != info.current)
            {
                auto current = display(owner->get());
                if (!current)
                    return cxx::unexpected(current.error());
                if (auto installed = install(std::move(*current)); !installed)
                    return installed;
            }
            if (compile_.value)
            {
                auto operation = services_.compilation.operation(compile_);
                if (operation)
                {
                    auto desired = operation->get().key();
                    desired.content = info.current;
                    if (services_.preview.status().desired.input != desired)
                        delivered_ = {}; // A discarded candidate may be requested again after Undo restores its source.
                    auto adoption = services_.preview.setDesired(desired);
                    if (!adoption)
                        return rejected(adoption.error());
                    const bool has_current_completion =
                        operation->get().ready() && operation->get().key() == desired && delivered_ != compile_;
                    if (has_current_completion)
                    {
                        auto received = services_.preview.receive(*adoption, operation->get().result(), compile_assets_);
                        if (received || !temporary(VMaterialViewFailure{received.error()}))
                            delivered_ = compile_;
                        if (!received)
                            return rejected(received.error());
                    }
                }
            }
            const auto instance = services_.preview.instance();
            const auto preview = services_.preview.status();
            const bool same_source = preview.accepted && preview.accepted->input.content.session == binding_->session.id();
            if (same_source && instance.valid() && instance != presented_)
            {
                auto candidate = lux::editor::views::ViewportPresentation::create(
                    services_.runtime,
                    instance,
                    services_.resources,
                    services_.render_system,
                    state_.camera.transform,
                    state_.camera.camera,
                    lux::scene::ViewConfig{.extent = state_.extent}
                );
                if (!candidate)
                    return rejected(candidate.error());
                viewport_.setPresentation(std::move(*candidate), state_.extent);
                presented_ = instance;
            }
            return {};
        }
    };
    MaterialView::MaterialView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        MaterialViewServices services,
        MaterialViewState state
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.material"}, "Material"),
          impl_(std::make_unique<Impl>(*this, services, state))
    {}
    MaterialView::~MaterialView() noexcept = default;
    MaterialViewResult<std::unique_ptr<MaterialView>> MaterialView::create(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        MaterialViewServices services,
        std::optional<MaterialViewBinding> binding,
        MaterialViewState state
    )
    {
        auto result = std::unique_ptr<MaterialView>(new MaterialView(dispatcher, std::move(id), services, state));
        if (!result->impl_->status_)
            return cxx::unexpected(result->impl_->status_.error());
        auto bound = result->rebind(binding);
        if (!bound)
            return cxx::unexpected(bound.error());
        return result;
    }
    MaterialViewResult<void> MaterialView::rebind(std::optional<MaterialViewBinding> binding)
    {
        return impl_->rebind(binding);
    }
    const std::optional<MaterialViewBinding>& MaterialView::binding() const noexcept
    {
        return impl_->binding_;
    }
    const MaterialViewState& MaterialView::state() const noexcept
    {
        return impl_->state_;
    }
    views::ViewCaptureResult MaterialView::captureState() const
    {
        workspace::VersionedViewState result;
        serialization::BinaryWriter writer(result.bytes);
        views::detail::writeViewportState(writer, impl_->state_.camera, impl_->state_.extent);
        return result;
    }
    views::ViewStateResult MaterialView::prepareState(std::uint32_t schema, std::span<const std::byte> bytes)
    {
        if (schema != 1)
            return cxx::unexpected(views::ViewPreparationFailure{"material.view.state", schema, "Unknown schema", false}
            );
        if (bytes.empty())
            return cxx::move_only_function<void()>{};
        serialization::BinaryReader reader(bytes);
        MaterialViewState candidate;
        if (!views::detail::readViewportState(reader, candidate.camera, candidate.extent) || reader.remaining())
            return cxx::unexpected(
                views::ViewPreparationFailure{"material.view.state", schema, "Invalid camera state", false}
            );
        return cxx::move_only_function<void()>{[this, candidate]() noexcept {
            impl_->state_ = candidate;
            impl_->camera_pending_ = true;
        }};
    }
    const MaterialViewResult<void>& MaterialView::status() const noexcept
    {
        return impl_->status_;
    }
    render::RTextureHandle MaterialView::image() const noexcept
    {
        return impl_->viewport_.image().image();
    }
    MaterialViewResult<void> MaterialView::rebindContent(const views::ViewContent& content)
    {
        const bool is_single = content.sessions.size() == 1 && content.primary == content.sessions.front();
        const bool is_invalid = !content.valid() || (!content.sessions.empty() && !is_single);
        if (is_invalid)
            return rejected(views::EViewError::INVALID_ID);
        if (impl_->binding_ && is_single && impl_->binding_->session.id() == *content.primary)
            return {};
        std::unique_ptr<MaterialInteraction> interaction;
        std::optional<MaterialViewBinding> binding;
        if (is_single)
        {
            auto key = impl_->services_.sessions.key(*content.primary);
            if (!key)
                return rejected(MaterialEditError{key.error()});
            interaction = std::make_unique<MaterialInteraction>(impl_->services_.sessions, *key);
            binding.emplace(*key, interaction.get());
        }
        auto adopted = rebind(binding);
        if (!adopted)
            return adopted;
        impl_->interaction_ = std::move(interaction);
        return {};
    }
    MaterialViewResult<void> MaterialView::beginEdit(std::string label)
    {
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        return accepted(impl_->binding_->interaction->begin(std::move(label)));
    }
    MaterialViewResult<void> MaterialView::previewEdit(std::vector<VMaterialEdit>& edits)
    {
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        return accepted(impl_->binding_->interaction->preview(edits));
    }
    MaterialViewResult<void> MaterialView::commitEdit()
    {
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        return accepted(impl_->binding_->interaction->commit());
    }
    MaterialViewResult<void> MaterialView::cancelEdit()
    {
        if (impl_->binding_)
        {
            auto ended = impl_->binding_->interaction->cancel();
            if (!ended)
                return rejected(ended.error());
        }
        return impl_->discardInputs();
    }
    MaterialViewResult<void> MaterialView::undo()
    {
        auto ended = cancelEdit();
        if (!ended)
            return ended;
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        auto owner = impl_->services_.sessions.edit(impl_->binding_->session);
        if (!owner)
            return rejected(MaterialEditError{owner.error()});
        return accepted(owner->get().undo());
    }
    MaterialViewResult<void> MaterialView::redo()
    {
        auto ended = cancelEdit();
        if (!ended)
            return ended;
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        auto owner = impl_->services_.sessions.edit(impl_->binding_->session);
        if (!owner)
            return rejected(MaterialEditError{owner.error()});
        return accepted(owner->get().redo());
    }
    MaterialViewResult<MaterialCompileId> MaterialView::compile()
    {
        if (!impl_->binding_)
            return rejected(views::EViewError::INVALID_ID);
        auto owner = impl_->services_.sessions.read(impl_->binding_->session);
        if (!owner)
            return rejected(MaterialEditError{owner.error()});
        auto snapshot = owner->get().capture();
        if (!snapshot)
            return rejected(snapshot.error());
        auto started = impl_->services_.compilation.start(
            std::move(*snapshot),
            {},
            impl_->services_.environment.version
        );
        if (!started)
            return rejected(started.error());
        if (!impl_->services_.compilation.releaseResult(impl_->compile_))
            std::terminate();
        impl_->compile_ = *started;
        impl_->compile_assets_ = impl_->services_.environment.assets;
        const auto operation = impl_->services_.compilation.operation(*started);
        auto adoption = impl_->services_.preview.setDesired(operation->get().key());
        if (!adoption)
            return rejected(adoption.error());
        return *started;
    }
    MaterialCompileId MaterialView::compilation() const noexcept
    {
        return impl_->compile_;
    }
    MaterialViewResult<void> MaterialView::requestPublication()
    {
        auto operation = impl_->services_.compilation.operation(impl_->compile_);
        if (!operation)
            return rejected(operation.error());
        auto compiled = operation->get().result();
        if (!compiled)
            return rejected(compiled.error());
        auto artifact = captureMaterialArtifact(std::move(*compiled));
        if (!artifact)
            return rejected(artifact.error());
        auto sent = emit(publishRequested, *artifact);
        if (!sent.complete())
            return rejected(views::EViewError::BUSY);
        return {};
    }

    MaterialViewResult<void> MaterialView::navigate(const lux::editor::views::CameraMotion& motion)
    {
        auto next =
            lux::editor::views::navigateCamera(impl_->state_.camera.transform, impl_->state_.camera.camera, motion);
        if (!next)
            return rejected(next.error());
        if (impl_->viewport_.bound())
        {
            auto adopted = impl_->viewport_.presentation().setCameraPose(next->transform, next->camera);
            if (!adopted)
                return rejected(adopted.error());
        }
        impl_->state_.camera = *next;
        return {};
    }
    MaterialPreviewStatus MaterialView::previewStatus() const
    {
        return impl_->services_.preview.status();
    }
    void MaterialView::update() noexcept
    {
        if (impl_->preview_owner_)
            impl_->preview_owner_->update();
        if (auto result = impl_->maintain(); !result)
            impl_->status_ = cxx::unexpected(result.error());
    }
}

namespace lux::editor::material
{
    MaterialViewResult<views::DetachedView> makeMaterialView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        MaterialViewServices services,
        std::optional<MaterialViewBinding> binding,
        MaterialViewState state
    )
    {
        auto created = MaterialView::create(dispatcher, std::move(id), services, binding, state);
        if (!created)
            return cxx::unexpected(created.error());
        const auto cancel = +[](lux::ui::Pane& pane) -> views::ViewCloseResult {
            auto ended = static_cast<MaterialView&>(pane).cancelEdit();
            if (!ended)
            {
                const auto* edit = std::get_if<MaterialEditError>(&ended.error());
                const bool is_session = edit && edit->code == EMaterialEditError::SESSION;
                const auto* view = std::get_if<views::EViewError>(&ended.error());
                const auto code = is_session ? static_cast<std::uint64_t>(edit->session)
                                  : edit     ? static_cast<std::uint64_t>(edit->code)
                                             : static_cast<std::uint64_t>(*view);
                return cxx::unexpected(views::ViewPreparationFailure{
                    is_session ? "session"
                    : edit     ? "material.edit"
                               : "view",
                    code,
                    "Interaction could not be ended",
                    temporary(ended.error())
                });
            }
            return {};
        };
        return views::DetachedView{
            contracts::CodeLease::builtin(),
            std::move(*created),
            cancel,
            cancel,
            +[](lux::ui::Pane& pane, std::uint32_t schema, std::span<const std::byte> bytes) {
                return static_cast<MaterialView&>(pane).prepareState(schema, bytes);
            },
            +[](const lux::ui::Pane& pane) { return static_cast<const MaterialView&>(pane).captureState(); },
            +[](const lux::ui::Pane& pane) noexcept -> views::ViewContent {
                const auto& binding = static_cast<const MaterialView&>(pane).binding();
                return binding ? views::ViewContent{{binding->session.id()}, binding->session.id()} : views::ViewContent{};
            },
            +[](lux::ui::Pane& pane, const views::ViewContent& content) -> views::ViewCloseResult {
                auto adopted = static_cast<MaterialView&>(pane).rebindContent(content);
                if (adopted)
                    return {};
                return cxx::unexpected(workbench::detail::viewPreparationFailure(adopted.error(), temporary(adopted.error())));
            }
        };
    }
    MaterialViewResult<views::DetachedView> makeMaterialContentView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        sessions::TSessionAccess<MaterialSession> sessions,
        lux::scene::SceneRuntime& runtime,
        MaterialCompilationService& compilation,
        const scene::ProjectionEnvironment& environment,
        std::span<const render::RenderFeatureRegistration> features,
        project::ProjectCatalogModel* assets,
        const views::ViewContent& content
    )
    {
        if (!environment.renderer || !environment.resources)
            return rejected(views::EViewError::NOT_ATTACHED);
        auto preview = std::make_unique<MaterialPreview>(
            runtime, MaterialPreviewEnvironment{environment, {features.begin(), features.end()}}
        );
        MaterialViewState state;
        state.camera.transform.translation = {0, 0, 3.5F};
        auto candidate = makeMaterialView(
            dispatcher, std::move(id),
            {sessions, runtime, *environment.resources, *environment.renderer, *preview, compilation,
             environment, assets, {2}},
            {}, state
        );
        if (!candidate)
            return candidate;
        auto& view = static_cast<MaterialView&>(*candidate->pane());
        view.impl_->preview_owner_ = std::move(preview);
        auto bound = view.rebindContent(content);
        if (!bound)
            return cxx::unexpected(bound.error());
        return candidate;
    }

}
