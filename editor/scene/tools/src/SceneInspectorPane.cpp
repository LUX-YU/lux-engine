#include <imgui.h>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/SceneInspectorPane.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/ui/Element.hpp>

#include <array>
#include <cstring>
#include <limits>

namespace lux::editor
{
    namespace
    {
        namespace ecs = simulation::ecs;
        using VInspectorValue = std::variant<ecs::Transform3D, scene::Camera, ecs::Mesh3D, ecs::Light3D>;

        template <class T> void fields(T&, float) noexcept;

        void assetField(const char* label, asset::AssetId& value) noexcept
        {
            std::array<char, 64> text{};
            constexpr char hex[] = "0123456789abcdef";
            std::size_t cursor = 0, index = 0;
            for (auto byte : value.bytes())
            {
                if (index == 4 || index == 6 || index == 8 || index == 10)
                {
                    text[cursor++] = '-';
                }
                const auto digit = std::to_integer<unsigned>(byte);
                text[cursor++] = hex[digit >> 4];
                text[cursor++] = hex[digit & 15];
                ++index;
            }
            // Commit only a complete identity; a partial typed UUID never replaces the draft.
            if (ImGui::InputText(label, text.data(), text.size(), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (auto parsed = uuids::uuid::from_string(text.data()))
                {
                    value = asset::AssetId{*parsed};
                }
            }
        }

        template <class T> void field(const char* label, T& value, float speed) noexcept
        {
            if constexpr (std::same_as<T, bool>)
            {
                ImGui::Checkbox(label, &value);
            }
            else if constexpr (std::same_as<T, double>)
            {
                ImGui::DragScalar(label, ImGuiDataType_Double, &value, speed);
            }
            else if constexpr (std::same_as<T, float>)
            {
                ImGui::DragFloat(label, &value, speed);
            }
            else if constexpr (std::same_as<T, Eigen::Vector3d>)
            {
                ImGui::DragScalarN(label, ImGuiDataType_Double, value.data(), 3, speed);
            }
            else if constexpr (std::same_as<T, Eigen::Quaterniond>)
            {
                Eigen::Vector3d angles = value.toRotationMatrix().eulerAngles(0, 1, 2);
                if (ImGui::DragScalarN(label, ImGuiDataType_Double, angles.data(), 3, speed))
                {
                    value = Eigen::AngleAxisd{angles.x(), Eigen::Vector3d::UnitX()} *
                            Eigen::AngleAxisd{angles.y(), Eigen::Vector3d::UnitY()} *
                            Eigen::AngleAxisd{angles.z(), Eigen::Vector3d::UnitZ()};
                }
            }
            else if constexpr (std::same_as<T, rdesc::MeshVisualDescription>)
            {
                assetField("Mesh asset", value.mesh);
                assetField("Material asset", value.material);
                ImGui::Checkbox("Visible", &value.visible);
                ImGui::Checkbox("Cast shadow", &value.cast_shadow);
                ImGui::Checkbox("Receive shadow", &value.receive_shadow);
            }
            else if constexpr (std::same_as<T, rdesc::LightDescription>)
            {
                int kind = static_cast<int>(value.type);
                if (ImGui::Combo("Type", &kind, "Directional\0Point\0Spot\0Area\0"))
                {
                    value.type = static_cast<rdesc::ELightType>(kind);
                }
                ImGui::ColorEdit3("Color", value.color.data());
                ImGui::DragFloat("Intensity", &value.intensity, 0.1F, 0, 1.0e6F);
                ImGui::DragFloat("Range", &value.range, 0.1F, 0.001F, 1.0e6F);
                ImGui::DragFloat("Inner cone (radians)", &value.inner_cone_angle, 0.01F, 0, 3.14F);
                ImGui::DragFloat("Outer cone (radians)", &value.outer_cone_angle, 0.01F, 0, 3.14F);
                ImGui::Checkbox("Cast shadow", &value.cast_shadow);
                ImGui::InputScalar("Shadow resolution", ImGuiDataType_U32, &value.shadow_map_size);
                ImGui::DragFloat("Shadow bias", &value.shadow_bias, 0.0001F);
            }
            else if constexpr (requires { std::variant_size<T>::value; })
            {
                int selected = static_cast<int>(value.index());
                if (ImGui::Combo(label, &selected, "Perspective\0Orthographic\0"))
                {
                    if (selected == 0)
                    {
                        value = scene::PerspectiveProjection{};
                    }
                    else
                    {
                        value = scene::OrthographicProjection{};
                    }
                }
                std::visit([&](auto& projection) noexcept { fields(projection, speed); }, value);
            }
            else if constexpr (meta::HasTypeStaticInfo<T>)
            {
                fields(value, speed);
            }
            else
            {
                ImGui::Text("%s (read-only type)", label);
            }
        }

        template <class T> void fields(T& value, float) noexcept
        {
            std::apply(
                [&](const auto&... metadata) noexcept
                {
                    const auto show = [&](const auto& member) noexcept
                    {
                        ImGui::PushID(member.name.data());
                        ImGui::BeginDisabled(member.read_only);
                        field(member.display_name.data(), value.*member.pointer, static_cast<float>(member.edit_speed));
                        ImGui::EndDisabled();
                        ImGui::PopID();
                    };
                    (show(metadata), ...);
                },
                meta::TTypeStaticInfo<T>::fields
            );
        }
    } // namespace

    struct SceneInspectorPane::Impl final : ui::Element
    {
        struct Draft final
        {
            std::string label;
            VInspectorValue value;
        };

        ProjectScenes& scenes;
        execution::ExecutionRuntime& execution;
        object::Connection project_changed, project_opened, document_changed, selection_changed;
        object::ObjectId observed_document;
        bool waiting_for_edit{};
        asset::AssetId document;
        world::WorldObjectId selected;
        std::uint64_t based_on{};
        std::vector<Draft> drafts;
        std::vector<std::string> unsupported;
        std::optional<std::size_t> apply;
        std::optional<SceneEdit> pending;
        bool revert{}, read_only{};
        std::string status;

        Impl(ProjectScenes& scenes, execution::ExecutionRuntime& execution) noexcept
            : scenes(scenes), execution(execution)
        {
        }

        void refresh(SceneDocument& source) noexcept
        {
            document = source.id();
            selected = source.selection();
            based_on = source.revision();
            drafts.clear();
            unsupported.clear();
            status.clear();
            const auto object = source.object(selected);
            if (!object)
            {
                return;
            }
            for (const auto& component : object->components)
            {
                auto value = source.component(selected, component.schema);
                if (!value)
                {
                    unsupported.push_back(component.schema.name);
                    continue;
                }
                bool supported = false;
                const auto add = [&]<class T>() noexcept
                {
                    if (value->type() == cxx::typeToken<T>())
                    {
                        drafts.push_back({component.schema.name, *static_cast<const T*>(value->data())});
                        supported = true;
                    }
                };
                add.template operator()<ecs::Transform3D>();
                add.template operator()<scene::Camera>();
                add.template operator()<ecs::Mesh3D>();
                add.template operator()<ecs::Light3D>();
                if (!supported)
                {
                    unsupported.push_back(component.schema.name);
                }
            }
        }

        ui::SizeHint sizeHintContent() noexcept override
        {
            ui::SizeHint hint;
            hint.minimum = {180, 120};
            hint.preferred = {320, 500};
            return hint;
        }

        void draw() noexcept override
        {
            ImGui::BeginChild("properties", {rect().size.width, rect().size.height});
            ImGui::TextWrapped("%s", status.c_str());
            if (ImGui::Button("Revert / refresh draft"))
            {
                revert = true;
                pane().requestUpdate();
            }
            ImGui::BeginDisabled(read_only || pending.has_value());
            for (std::size_t i = 0; i < drafts.size(); ++i)
            {
                auto& draft = drafts[i];
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::CollapsingHeader(draft.label.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                {
                    std::visit([](auto& value) noexcept { fields(value, 0.01F); }, draft.value);
                    if (ImGui::Button("Apply component"))
                    {
                        apply = i;
                        pane().requestUpdate();
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndDisabled();
            for (const auto& name : unsupported)
            {
                ImGui::TextWrapped("%s: preserved; no field editor installed", name.c_str());
            }
            ImGui::EndChild();
        }
    };

    SceneInspectorPane::SceneInspectorPane(
        ProjectScenes& scenes,
        execution::ExecutionRuntime& execution,
        std::string title
    )
        : Pane(std::move(title)), impl_(std::make_unique<Impl>(scenes, execution))
    {
        auto changed = connect(&scenes, &ProjectScenes::changed, this, [this]() noexcept { requestUpdate(); });
        auto opened =
            connect(&scenes, &ProjectScenes::opened, this, [this](asset::AssetId) noexcept { requestUpdate(); });
        if (!changed || !opened)
        {
            std::terminate();
        }
        impl_->project_changed = std::move(*changed);
        impl_->project_opened = std::move(*opened);
        if (!addElement(*impl_))
        {
            std::terminate();
        }
    }

    SceneInspectorPane::~SceneInspectorPane() noexcept = default;

    void SceneInspectorPane::waitForEdit(SceneDocument& document) noexcept
    {
        if (impl_->waiting_for_edit)
        {
            return;
        }
        impl_->waiting_for_edit = true;
        const auto target = objectId();
        auto work =
            execution::taskResult(stdexec::starts_on(impl_->execution.mainScheduler(), document.whenEditable()));
        auto accepted = impl_->execution.spawn(stdexec::then(
            std::move(work),
            [target](auto&& result) noexcept
            {
                const auto found = object::ObjectRuntime::instance().resolve(target);
                if (!found)
                {
                    return;
                }
                auto& pane = *static_cast<SceneInspectorPane*>(*found);
                pane.impl_->waiting_for_edit = false;
                if (!result)
                {
                    pane.impl_->pending.reset();
                    pane.impl_->status = "Scene retired before the edit could be applied";
                }
                pane.requestUpdate();
            }
        ));
        if (!accepted)
        {
            impl_->waiting_for_edit = false;
            impl_->pending.reset();
            impl_->status = "Edit completion could not be scheduled";
        }
    }

    void SceneInspectorPane::update() noexcept
    {
        auto& state = *impl_;
        auto* document = state.scenes.find(state.scenes.activeDocument());
        if (!document)
        {
            state.document = {};
            state.observed_document = {};
            state.document_changed = {};
            state.selection_changed = {};
            state.drafts.clear();
            state.unsupported.clear();
            state.apply.reset();
            state.pending.reset();
            state.status = "Select a Scene object in Hierarchy or viewport";
            return;
        }
        if (state.observed_document != document->objectId())
        {
            state.observed_document = document->objectId();
            auto changed = connect(document, &SceneDocument::changed, this, [this]() noexcept { requestUpdate(); });
            auto selected =
                connect(document, &SceneDocument::selectionChanged, this, [this]() noexcept { requestUpdate(); });
            if (!changed || !selected)
            {
                std::terminate();
            }
            state.document_changed = std::move(*changed);
            state.selection_changed = std::move(*selected);
        }
        const bool changed_selection = state.document != document->id() || state.selected != document->selection();
        if (changed_selection || std::exchange(state.revert, false))
        {
            state.apply.reset();
            state.pending.reset();
            state.refresh(*document);
        }
        state.read_only = document->readOnly();
        if (state.pending && !state.waiting_for_edit)
        {
            const auto result = document->apply(*state.pending);
            if (!result && isSceneEditBusy(result.error()))
            {
                state.status = "Waiting for the Scene edit safe point";
                waitForEdit(*document);
                return;
            }
            state.pending.reset();
            if (result)
            {
                state.refresh(*document);
            }
            else
            {
                state.status = "Queued edit rejected; the original draft revision was preserved";
            }
        }
        if (document->revision() != state.based_on)
        {
            state.status = "Scene changed: this draft is stale. Revert before editing the new revision.";
        }
        if (auto apply = std::exchange(state.apply, {}))
        {
            bool applied = false;
            std::visit(
                [&](const auto& value) noexcept
                {
                    auto edit = document->prepareComponentEdit(state.selected, state.based_on, value);
                    auto result = edit ? document->apply(*edit) : SceneEditResult<void>{cxx::unexpected(edit.error())};
                    if (result)
                    {
                        applied = true;
                    }
                    else
                    {
                        if (edit && isSceneEditBusy(result.error()))
                        {
                            state.pending = std::move(*edit);
                            state.status = "Waiting for the Scene edit safe point";
                            waitForEdit(*document);
                        }
                        else
                        {
                            state.status = "Rejected: stale revision, invalid author value, or read-only Scene";
                        }
                    }
                },
                state.drafts[*apply].value
            );
            if (applied)
            {
                state.refresh(*document);
                state.status = "Component applied";
            }
        }
    }
} // namespace lux::editor
