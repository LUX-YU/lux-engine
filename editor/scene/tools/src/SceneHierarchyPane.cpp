#include <imgui.h>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>
#include <lux/engine/editor/SceneHierarchyPane.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/ui/Element.hpp>

#include <array>
#include <map>
#include <random>

namespace lux::editor
{
    struct SceneHierarchyPane::Impl final : ui::Element
    {
        struct Row final
        {
            world::WorldObjectId id, parent;
            std::string label;
            std::uint32_t depth{};
        };

        ProjectScenes& scenes;
        execution::ExecutionRuntime& execution;
        object::Connection project_changed, project_opened, document_changed, selection_changed;
        object::ObjectId observed_document;
        bool waiting_for_edit{};
        asset::AssetId document;
        std::uint64_t revision{UINT64_MAX};
        std::uint64_t hierarchy_revision{UINT64_MAX};
        std::vector<Row> rows;
        world::WorldObjectId selected, clicked;
        std::array<char, 64> parent_text{};
        bool erase{}, create{}, reparent{}, read_only{};
        std::optional<SceneEdit> pending;
        int create_kind{};
        std::string status;

        Impl(ProjectScenes& scenes, execution::ExecutionRuntime& execution) noexcept
            : scenes(scenes), execution(execution)
        {
        }

        void refresh(SceneDocument& source) noexcept
        {
            document = source.id();
            revision = source.revision();
            hierarchy_revision = source.hierarchyRevision();
            rows.clear();
            auto objects = source.objects();
            if (!objects)
            {
                return;
            }
            std::map<world::WorldObjectId, std::vector<world::WorldObjectId>, world::WorldObjectIdLess> children;
            for (auto id : *objects)
            {
                auto parent = source.parent(id);
                children[parent ? *parent : world::WorldObjectId{}].push_back(id);
            }
            // Iterative DFS avoids consuming the C++ call stack for deeply nested author hierarchies.
            std::vector<Row> pending;
            for (auto id : children[{}])
            {
                pending.push_back({id, {}, uuids::to_string(id.value), 0});
            }
            while (!pending.empty())
            {
                auto row = std::move(pending.back());
                pending.pop_back();
                if (const auto found = children.find(row.id); found != children.end())
                {
                    for (auto id : found->second)
                    {
                        pending.push_back({id, row.id, uuids::to_string(id.value), row.depth + 1});
                    }
                }
                rows.push_back(std::move(row));
            }
        }

        ui::SizeHint sizeHintContent() noexcept override
        {
            ui::SizeHint hint;
            hint.minimum = {160, 120};
            hint.preferred = {300, 500};
            return hint;
        }

        void draw() noexcept override
        {
            ImGui::BeginChild("hierarchy", {rect().size.width, rect().size.height});
            ImGui::TextWrapped("%s", status.c_str());
            ImGui::BeginDisabled(read_only || pending.has_value());
            ImGui::Combo("Create", &create_kind, "Empty object\0Camera\0Point light\0");
            if (ImGui::Button("Create object"))
            {
                create = true;
                pane().requestUpdate();
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete selected"))
            {
                erase = true;
                pane().requestUpdate();
            }
            ImGui::InputText("Parent UUID", parent_text.data(), parent_text.size());
            if (ImGui::Button("Set parent (empty = root)"))
            {
                reparent = true;
                pane().requestUpdate();
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto& row = rows[i];
                    ImGui::Indent(static_cast<float>(std::min(row.depth, 12U)) * 10.0F);
                    if (ImGui::Selectable(row.label.c_str(), selected == row.id))
                    {
                        clicked = row.id;
                        pane().requestUpdate();
                    }
                    ImGui::Unindent(static_cast<float>(std::min(row.depth, 12U)) * 10.0F);
                }
            }
            ImGui::EndChild();
        }

        SceneEditResult<SceneEdit> prepareObject(SceneDocument& source) noexcept
        {
            // The current creation UI offers single-partition authoring. Indexed worlds need a real placement query.
            if (source.baseline().world->data().partitioner().id.name != "lux.spatial.builtin.single")
            {
                return cxx::unexpected(VSceneEditFailure{ESceneEditError::NOT_RESIDENT});
            }
            std::mt19937 random{std::random_device{}()};
            uuids::uuid_random_generator generate{random};
            SceneObjectData object{{generate()}, {0}};
            const auto append = [&](const auto& value) -> SceneEditResult<void>
            {
                const auto* schema = source.schemas().find(cxx::typeToken<std::remove_cvref_t<decltype(value)>>());
                if (!schema || !schema->capture_value)
                {
                    return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
                }
                simulation::ecs::WorldEntityMap references;
                auto captured = schema->capture_value(&value, schema->code_lifetime);
                auto encoded = captured.encode(references, 4096);
                if (!encoded)
                {
                    return cxx::unexpected(VSceneEditFailure{encoded.error()});
                }
                object.components.push_back({schema->id, schema->version, std::move(*encoded)});
                return {};
            };
            auto result = append(simulation::ecs::Transform3D{});
            if (result && create_kind == 1)
            {
                result = append(scene::Camera{});
            }
            if (result && create_kind == 2)
            {
                result = append(simulation::ecs::Light3D{});
            }
            if (!result)
            {
                return cxx::unexpected(result.error());
            }
            SceneCreateObjects create;
            create.objects.push_back(std::move(object));
            return SceneEdit{revision, std::move(create)};
        }
    };

    SceneHierarchyPane::SceneHierarchyPane(
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

    SceneHierarchyPane::~SceneHierarchyPane() noexcept = default;

    void SceneHierarchyPane::waitForEdit(SceneDocument& document) noexcept
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
                auto& pane = *static_cast<SceneHierarchyPane*>(*found);
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

    void SceneHierarchyPane::update() noexcept
    {
        auto& state = *impl_;
        auto* document = state.scenes.find(state.scenes.activeDocument());
        if (!document)
        {
            state.rows.clear();
            state.document = {};
            state.observed_document = {};
            state.document_changed = {};
            state.selection_changed = {};
            state.status = "Open a Scene";
            state.create = state.erase = state.reparent = false;
            state.clicked = {};
            state.pending.reset();
            return;
        }
        // Consume intent against the revision shown when it was produced; a refresh never rebases a command.
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
        const bool changed_document = state.document != document->id();
        if (changed_document)
        {
            state.create = state.erase = state.reparent = false;
            state.clicked = {};
            state.pending.reset();
        }
        const auto queue = [&](SceneEditResult<SceneEdit> edit) noexcept
        {
            if (edit)
            {
                state.pending = std::move(*edit);
            }
            else
            {
                state.status = "Cannot prepare this author edit";
            }
        };
        if (std::exchange(state.create, false))
        {
            queue(state.prepareObject(*document));
        }
        if (std::exchange(state.erase, false))
        {
            state.pending = SceneEdit{state.revision, SceneEraseObjects{{state.selected}}};
        }
        if (std::exchange(state.reparent, false))
        {
            const auto parent = state.parent_text[0] ? uuids::uuid::from_string(state.parent_text.data())
                                                     : std::optional<uuids::uuid>{uuids::uuid{}};
            if (parent)
            {
                queue(document->prepareParentEdit(state.selected, {*parent}, state.revision));
            }
            else
            {
                state.status = "Invalid parent UUID";
            }
        }
        if (state.pending && !state.waiting_for_edit)
        {
            const auto result = document->apply(*state.pending);
            if (!result && isSceneEditBusy(result.error()))
            {
                state.status = "Waiting for the Scene edit safe point";
                waitForEdit(*document);
            }
            else
            {
                if (result)
                {
                    if (const auto* create = std::get_if<SceneCreateObjects>(&state.pending->operation);
                        create && !create->objects.empty())
                    {
                        static_cast<void>(document->select(create->objects.front().id));
                    }
                    state.status = "Edit applied";
                }
                else
                {
                    state.status = "Edit rejected: inspect references, schema, read-only state or revision";
                }
                state.pending.reset();
            }
        }
        if (state.clicked.valid())
        {
            static_cast<void>(document->select(std::exchange(state.clicked, {})));
        }
        if (changed_document || state.hierarchy_revision != document->hierarchyRevision())
        {
            state.refresh(*document);
        }
        state.revision = document->revision();
        state.selected = document->selection();
        state.read_only = document->readOnly();
    }
} // namespace lux::editor
