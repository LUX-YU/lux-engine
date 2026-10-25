#include <imgui.h>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/SceneInspectorPane.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/ui/Element.hpp>

namespace lux::editor
{

    struct SceneInspectorPane::Impl final : ui::Element
    {
        struct Draft final
        {
            std::string label;
            simulation::ecs::DecodedComponent value;
            const ComponentEditor* editor;
        };

        EditorContext& context;
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

        Impl(EditorContext& context, ProjectScenes& scenes) noexcept
            : context(context), scenes(scenes), execution(context.engine().execution())
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
                const auto* editor = context.componentEditor(value->type());
                if (editor)
                {
                    drafts.push_back({component.schema.name, std::move(*value), editor});
                }
                else
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
                    draft.editor->draw(draft.value.data());
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

    SceneInspectorPane::SceneInspectorPane(EditorContext& context, ProjectScenes& scenes, std::string title)
        : Pane(std::move(title)), impl_(std::make_unique<Impl>(context, scenes))
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
            auto edit = document->prepareComponentEdit(state.selected, state.based_on, state.drafts[*apply].value);
            auto result = edit ? document->apply(*edit) : SceneEditResult<void>{cxx::unexpected(edit.error())};
            if (result)
            {
                applied = true;
            }
            else if (edit && isSceneEditBusy(result.error()))
            {
                state.pending = std::move(*edit);
                state.status = "Waiting for the Scene edit safe point";
                waitForEdit(*document);
            }
            else
            {
                state.status = "Rejected: stale revision, invalid author value, or read-only Scene";
            }
            if (applied)
            {
                state.refresh(*document);
                state.status = "Component applied";
            }
        }
    }
} // namespace lux::editor
