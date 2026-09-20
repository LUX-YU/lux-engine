#include <imgui.h>
#include <lux/engine/editor/gui/scene/OutlinerPane.hpp>
#include <lux/engine/editor/gui/scene/UiMeasurement.hpp>
#include <lux/engine/ui/Frame.hpp>

#include <cstring>
#include <unordered_map>

namespace lux::editor::gui
{
    OutlinerPane::OutlinerPane(scene::SceneEditor &document, std::string id)
        : DocumentPane(document, std::move(id), "Outliner"), selection_(document.selection()),
          selection_connection_(document.observeScoped<scene::SceneEditor::selectionChanged>(
              [this](const scene::SelectionNotice &value) noexcept
              {
                  selection_ = value;
                  LUX_UI_MEASURE(std::printf("D3_DIAGNOSTIC Outliner selection pane=%p revision=%llu\n",
                                             static_cast<void *>(this),
                                             static_cast<unsigned long long>(value.revision)));
              })),
          objects_connection_(document.observeScoped<scene::SceneEditor::objectsChanged>(
              [this](editing::Revision) noexcept { rows_dirty_ = true; }))
    {
    }

    void OutlinerPane::rebuildRows()
    {
        const auto objects = document_.objects();
        const auto none = objects.size();
        std::unordered_map<scene::SceneEntityRef, std::size_t, scene::SceneEntityRef::Hash> by_id;
        by_id.reserve(objects.size());
        for (std::size_t index{}; index < objects.size(); ++index)
        {
            by_id.emplace(objects[index].object, index);
        }

        // The extra head contains roots. Parent support comes from the document, never from the Pane.
        std::vector<std::size_t> first(objects.size() + 1, none), next(objects.size(), none);
        for (auto index = objects.size(); index-- > 0;)
        {
            const auto parent = by_id.find(objects[index].parent);
            const auto head = parent == by_id.end() ? none : parent->second;
            next[index] = first[head];
            first[head] = index;
        }

        rows_.clear();
        rows_.reserve(objects.size());
        std::vector<bool> visited(objects.size());
        std::vector<std::size_t> parents;
        auto source = first[none];
        std::size_t depth{}, orphan{};
        while (rows_.size() < objects.size())
        {
            if (source == none)
            {
                if (!parents.empty())
                {
                    source = next[rows_[parents.back()].source];
                    rows_[parents.back()].end = rows_.size();
                    parents.pop_back();
                    --depth;
                    continue;
                }
                while (orphan < objects.size() && visited[orphan])
                {
                    ++orphan;
                }
                source = orphan;
                if (source == none)
                {
                    break;
                }
            }
            if (visited[source])
            {
                source = none;
                continue;
            }
            visited[source] = true;
            const auto row = rows_.size();
            rows_.push_back({source, depth, row + 1});
            if (first[source] != none)
            {
                parents.push_back(row);
                source = first[source];
                ++depth;
            }
            else
            {
                source = next[source];
            }
        }
        for (const auto row : parents)
        {
            rows_[row].end = rows_.size();
        }
        std::erase_if(collapsed_, [&](const auto &id) { return !by_id.contains(id); });
        rows_dirty_ = false;
        visible_dirty_ = true;
    }

    void OutlinerPane::rebuildVisibleRows()
    {
        const auto objects = document_.objects();
        visible_rows_.clear();
        visible_rows_.reserve(rows_.size());
        for (std::size_t index{}; index < rows_.size();)
        {
            visible_rows_.push_back(index);
            const auto &row = rows_[index];
            index = collapsed_.contains(objects[row.source].object) ? row.end : index + 1;
        }
        visible_dirty_ = false;
    }

    EditorResult<void> OutlinerPane::select(scene::SceneEntityRef object)
    {
        // Resolve an owned gesture before changing selection. Failure keeps its Pane
        // and draft intact, and leaves the selected object unchanged.
        for (const auto &view : document_.views())
        {
            if (auto *gui = dynamic_cast<GuiView *>(view.get()))
            {
                if (auto finished = gui->finishInteraction(); !finished)
                {
                    return finished;
                }
            }
        }
        return document_.select(object);
    }

    void OutlinerPane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
    {
        LUX_UI_MEASURE(UiMeasurement measurement{"Outliner"});
        LUX_UI_MEASURE(measurement.objects = document_.objects().size());
        LUX_UI_MEASURE(measurement.directory_rebuilds = rows_dirty_ ? 1 : 0);
        context.activateContext(lux::ui::UiContextIdView{id()});
        const auto run_state = document_.runStatus().state;
        const bool running = run_state == scene::ERunState::PREPARING || run_state == scene::ERunState::RUNNING ||
                             run_state == scene::ERunState::PAUSED || run_state == scene::ERunState::STOPPING;
        const bool structure_read_only = document_.summary().read_only || running;

        const auto record = [this](const auto &result)
        {
            error_.clear();
            if (!result)
            {
                const auto &failure = result.error();
                error_ = failure.message.data();
                if (error_.empty())
                {
                    error_ = "Edit rejected (" + std::to_string(static_cast<unsigned>(failure.code)) + ")";
                }
            }
        };
        const auto create = [&](scene::EObjectSpace space)
        {
            const auto history = document_.historyView();
            if (history)
            {
                record(document_.createObject(history->history.current, lux::partition::PartitionOrdinal{partition_},
                                              space));
            }
        };
        ImGui::BeginDisabled(structure_read_only || document_.partitionCount() == 0);
        if (ImGui::SmallButton("Create"))
        {
            ImGui::OpenPopup("create-object");
        }
        if (ImGui::BeginPopup("create-object"))
        {
            if (document_.partitionCount() > 1 && ImGui::BeginCombo("Partition", std::to_string(partition_).c_str()))
            {
                for (std::uint32_t ordinal{}; ordinal < document_.partitionCount(); ++ordinal)
                {
                    if (ImGui::Selectable(std::to_string(ordinal).c_str(), partition_ == ordinal))
                    {
                        partition_ = ordinal;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::MenuItem("Empty object"))
            {
                create(scene::EObjectSpace::NONE);
            }
            if (document_.supportsObjectSpace(scene::EObjectSpace::SPACE_2D) && ImGui::MenuItem("2D object"))
            {
                create(scene::EObjectSpace::SPACE_2D);
            }
            if (document_.supportsObjectSpace(scene::EObjectSpace::SPACE_3D) && ImGui::MenuItem("3D object"))
            {
                create(scene::EObjectSpace::SPACE_3D);
            }
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        if (rows_dirty_)
        {
            rebuildRows();
        }
        if (visible_dirty_)
        {
            rebuildVisibleRows();
        }
        selection_ = document_.selection();
        frame.textMuted(running ? "Run objects" : "Author objects");
        const auto objects = document_.objects();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible_rows_.size()));
        while (clipper.Step())
        {
            for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible)
            {
                const auto index = visible_rows_[static_cast<std::size_t>(visible)];
                const auto row = rows_[index];
                const auto object = objects[row.source];
                LUX_UI_MEASURE(++measurement.rows);
                const std::array<std::uint64_t, 2> identity{object.object.instance.value,
                                                          lux::simulation::ecs::entityBits(object.object.entity)};
                const auto bytes = std::as_bytes(std::span(identity));
                const auto *id = reinterpret_cast<const char *>(bytes.data());
                ImGui::PushID(id, id + bytes.size());
                const float indent = ImGui::GetStyle().IndentSpacing * static_cast<float>(row.depth);
                if (row.depth)
                {
                    ImGui::Indent(indent);
                }
                auto flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_DefaultOpen;
                if (row.end == index + 1)
                {
                    flags |= ImGuiTreeNodeFlags_Leaf;
                }
                if (object.object == selection_.object)
                {
                    flags |= ImGuiTreeNodeFlags_Selected;
                }
                const bool was_open = !collapsed_.contains(object.object);
                ImGui::SetNextItemOpen(was_open, ImGuiCond_Always);
                const bool open = ImGui::TreeNodeEx("object", flags, "%s", object.label.c_str());
                if (open != was_open)
                {
                    if (open)
                    {
                        collapsed_.erase(object.object);
                    }
                    else
                    {
                        collapsed_.insert(object.object);
                    }
                    visible_dirty_ = true;
                }
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                {
                    record(select(object.object));
                }
                constexpr char object_payload[] = "lux.scene.object.v1";
                static_assert(std::is_trivially_copyable_v<scene::SceneWriteTarget>);
                if (!structure_read_only && document_.supportsHierarchy() && ImGui::BeginDragDropSource())
                {
                    const auto target = document_.writeTarget(object.object);
                    if (target)
                    {
                        ImGui::SetDragDropPayload(object_payload, &*target, sizeof(*target));
                        ImGui::TextUnformatted(object.label.c_str());
                    }
                    ImGui::EndDragDropSource();
                }
                if (!structure_read_only && document_.supportsHierarchy() && ImGui::BeginDragDropTarget())
                {
                    if (const auto *payload = ImGui::AcceptDragDropPayload(object_payload))
                    {
                        if (payload->DataSize == sizeof(scene::SceneWriteTarget))
                        {
                            scene::SceneWriteTarget target;
                            std::memcpy(&target, payload->Data, sizeof(target));
                            record(document_.reparent(target, object.object));
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::BeginPopupContextItem("object-actions"))
                {
                    ImGui::BeginDisabled(structure_read_only);
                    if (ImGui::MenuItem("Delete object"))
                    {
                        record(document_.eraseObjects(document_.historyView()->history.current,
                                                      std::span(&object.object, 1)));
                    }
                    else if (row.end > index + 1 && ImGui::MenuItem("Delete subtree"))
                    {
                        std::vector<scene::SceneEntityRef> removed;
                        removed.reserve(row.end - index);
                        for (auto child = index; child < row.end; ++child)
                        {
                            removed.push_back(objects[rows_[child].source].object);
                        }
                        record(document_.eraseObjects(document_.historyView()->history.current, removed));
                    }
                    else if (object.parent.valid() && ImGui::MenuItem("Detach from parent"))
                    {
                        const auto target = document_.writeTarget(object.object);
                        if (target)
                        {
                            record(document_.reparent(*target, {}));
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::EndPopup();
                }
                if (row.depth)
                {
                    ImGui::Unindent(indent);
                }
                ImGui::PopID();
                if (rows_dirty_)
                {
                    // A structural action invalidates this frame's row borrows. Rebuild at the next draw.
                    break;
                }
            }
            if (rows_dirty_ || visible_dirty_)
            {
                break;
            }
        }
        if (frame.smallButton("Clear selection"))
        {
            record(select({}));
        }
        if (!error_.empty())
        {
            frame.text(error_);
        }
    }
} // namespace lux::editor::gui
