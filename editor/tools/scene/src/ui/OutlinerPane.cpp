#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <imgui.h>
#include <lux/engine/editor/ui/scene/OutlinerPane.hpp>
#include <lux/engine/editor/ui/scene/UiMeasurement.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/scene/RenderViewRequest.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>

#include <lux/engine/editor/scene/detail/EditorEntity.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <cstring>
#include <unordered_map>

namespace lux::editor::ui
{
    OutlinerElement::OutlinerElement(
        lux::ui::Pane& parent,
        scene::SceneEditor::Impl& editor,
        lux::scene::SceneRuntime& runtime,
        EditorResult<void>& status
    )
        : lux::ui::Element(parent, lux::ui::ElementId{"objects"}), editor_(editor), runtime_(runtime),
          selection_(editor.selection()), selection_connection_(lux::editor::detail::takeConnection(
                                              lux::object::LuxObject::connect(
                                                  editor.editor,
                                                  &scene::SceneEditor::selectionChanged,
                                                  [this](const scene::SelectionNotice& value) noexcept {
                                                      selection_ = value;
                                                      LUX_UI_MEASURE(std::printf(
                                                          "D3_DIAGNOSTIC Outliner selection pane=%p revision=%llu\n",
                                                          static_cast<void*>(this),
                                                          static_cast<unsigned long long>(value.revision)
                                                      ));
                                                  }
                                              ),
                                              status
                                          )),
          objects_connection_(lux::editor::detail::takeConnection(
              lux::object::LuxObject::connect(
                  editor.editor,
                  &scene::SceneEditor::objectsChanged,
                  [this](editing::Revision) noexcept { rows_dirty_ = true; }
              ),
              status
          ))
    {}

    void OutlinerElement::rebuildRows()
    {
        objects_.clear();
        if (const auto borrowed = std::as_const(runtime_).borrowInstance(editor_.instance()))
        {
            namespace ecs = lux::simulation::ecs;
            const auto& registry = borrowed->get();
            for (const auto [entity] : registry.storage<ecs::Entity>()->each())
            {
                const auto* request = registry.try_get<lux::scene::RenderViewRequest>(entity);
                const bool is_transient_view = request && request->destroy_entity_on_stop;
                const bool is_editor_entity = registry.all_of<scene::detail::EditorEntity>(entity);
                if (is_transient_view || is_editor_entity)
                    continue;
                const auto* parent = registry.try_get<ecs::Parent>(entity);
                objects_.push_back(
                    {entity,
                     parent ? parent->entity : ecs::NullEntity,
                     "Object " + std::to_string(ecs::entityBits(entity))}
                );
            }
            std::ranges::sort(objects_, {}, &ObjectRow::object);
        }
        const auto& objects = objects_;
        const auto none = objects.size();
        std::unordered_map<lux::simulation::ecs::Entity, std::size_t> by_id;
        by_id.reserve(objects.size());
        for (std::size_t index{}; index < objects.size(); ++index)
        {
            by_id.emplace(objects[index].object, index);
        }

        // The extra head contains roots; hierarchy facts were read directly from the Registry.
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
        std::erase_if(collapsed_, [&](const auto& id) { return !by_id.contains(id); });
        rows_dirty_ = false;
        visible_dirty_ = true;
    }

    void OutlinerElement::rebuildVisibleRows()
    {
        const auto& objects = objects_;
        visible_rows_.clear();
        visible_rows_.reserve(rows_.size());
        for (std::size_t index{}; index < rows_.size();)
        {
            visible_rows_.push_back(index);
            const auto& row = rows_[index];
            index = collapsed_.contains(objects[row.source].object) ? row.end : index + 1;
        }
        visible_dirty_ = false;
    }

    EditorResult<void> OutlinerElement::select(lux::simulation::ecs::Entity object)
    {
        selection_request_ = object;
        return {};
    }
    void OutlinerElement::update() noexcept
    {
        if (editor_.instance() != observed_instance_)
        {
            selection_request_.reset();
            collapsed_.clear();
        }
        if (selection_request_)
        {
            const auto finished = editor_.finishEditing();
            if (finished)
            {
                const auto selected = editor_.select(*selection_request_);
                if (selected)
                    selection_request_.reset();
                else
                    error_ = selected.error().message;
            }
            else
                error_ = finished.error().message;
        }
        const auto history = editor_.historyView();
        const auto instance = editor_.instance();
        if (instance != observed_instance_ || (history && history->history.revision != observed_revision_))
        {
            rows_dirty_ = true;
            observed_instance_ = instance;
            if (history)
                observed_revision_ = history->history.revision;
        }
        if (rows_dirty_)
            rebuildRows();
        if (visible_dirty_)
            rebuildVisibleRows();
        selection_ = editor_.selection();
    }

    void OutlinerElement::draw() noexcept
    {
        if (ImGui::BeginChild("outliner-content", {rect().size.width, rect().size.height}))
            drawObjects();
        ImGui::EndChild();
    }

    void OutlinerElement::drawObjects() noexcept
    {
        LUX_UI_MEASURE(UiMeasurement measurement{"Outliner"});
        LUX_UI_MEASURE(measurement.objects = objects_.size());
        LUX_UI_MEASURE(measurement.directory_rebuilds = rows_dirty_ ? 1 : 0);

        const auto run_state = editor_.runStatus().state;
        const bool running = run_state == scene::EPlaybackState::PREPARING ||
                             run_state == scene::EPlaybackState::RUNNING ||
                             run_state == scene::EPlaybackState::PAUSED || run_state == scene::EPlaybackState::STOPPING;
        const bool structure_read_only = !editor_.writeRestriction().empty() || running;

        const auto record = [this](const auto& result) {
            error_.clear();
            if (!result)
            {
                const auto& failure = result.error();
                error_ = failure.message.data();
                if (error_.empty())
                {
                    error_ = "Edit rejected (" + std::to_string(static_cast<unsigned>(failure.code)) + ")";
                }
            }
        };
        const auto create = [&](scene::EObjectSpace space) {
            const auto history = editor_.historyView();
            if (history)
            {
                record(
                    editor_.createObject(history->history.current, lux::partition::PartitionOrdinal{partition_}, space)
                );
            }
        };
        ImGui::BeginDisabled(structure_read_only || editor_.partitionCount() == 0);
        if (ImGui::SmallButton("Create"))
        {
            ImGui::OpenPopup("create-object");
        }
        if (ImGui::BeginPopup("create-object"))
        {
            if (editor_.partitionCount() > 1 && ImGui::BeginCombo("Partition", std::to_string(partition_).c_str()))
            {
                for (std::uint32_t ordinal{}; ordinal < editor_.partitionCount(); ++ordinal)
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
            if (editor_.supportsObjectSpace(scene::EObjectSpace::SPACE_2D) && ImGui::MenuItem("2D object"))
            {
                create(scene::EObjectSpace::SPACE_2D);
            }
            if (editor_.supportsObjectSpace(scene::EObjectSpace::SPACE_3D) && ImGui::MenuItem("3D object"))
            {
                create(scene::EObjectSpace::SPACE_3D);
            }
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        {
            const auto& message_value = running ? "Run objects" : "Author objects";
            const std::string_view message{message_value};
            ImGui::TextDisabled("%.*s", static_cast<int>(message.size()), message.empty() ? "" : message.data());
        }
        const auto& objects = objects_;
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
                const std::array<std::uint64_t, 3> identity{
                    observed_instance_.domain,
                    (std::uint64_t{observed_instance_.slot} << 32) | observed_instance_.generation,
                    lux::simulation::ecs::entityBits(object.object)
                };
                const auto bytes = std::as_bytes(std::span(identity));
                const auto* id = reinterpret_cast<const char*>(bytes.data());
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
                if (!structure_read_only && editor_.supportsHierarchy() && ImGui::BeginDragDropSource())
                {
                    const auto target = editor_.writeTarget(object.object);
                    if (target)
                    {
                        ImGui::SetDragDropPayload(object_payload, &*target, sizeof(*target));
                        ImGui::TextUnformatted(object.label.c_str());
                    }
                    ImGui::EndDragDropSource();
                }
                if (!structure_read_only && editor_.supportsHierarchy() && ImGui::BeginDragDropTarget())
                {
                    if (const auto* payload = ImGui::AcceptDragDropPayload(object_payload))
                    {
                        if (payload->DataSize == sizeof(scene::SceneWriteTarget))
                        {
                            scene::SceneWriteTarget target;
                            std::memcpy(&target, payload->Data, sizeof(target));
                            record(editor_.reparent(target, object.object));
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::BeginPopupContextItem("object-actions"))
                {
                    ImGui::BeginDisabled(structure_read_only);
                    if (ImGui::MenuItem("Delete object"))
                    {
                        record(
                            editor_.eraseObjects(editor_.historyView()->history.current, std::span(&object.object, 1))
                        );
                    }
                    else if (row.end > index + 1 && ImGui::MenuItem("Delete subtree"))
                    {
                        std::vector<lux::simulation::ecs::Entity> removed;
                        removed.reserve(row.end - index);
                        for (auto child = index; child < row.end; ++child)
                        {
                            removed.push_back(objects[rows_[child].source].object);
                        }
                        record(editor_.eraseObjects(editor_.historyView()->history.current, removed));
                    }
                    else if (object.parent != lux::simulation::ecs::NullEntity && ImGui::MenuItem("Detach from parent"))
                    {
                        const auto target = editor_.writeTarget(object.object);
                        if (target)
                        {
                            record(editor_.reparent(*target, lux::simulation::ecs::NullEntity));
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
        if (ImGui::SmallButton("Clear selection"))
        {
            record(select(lux::simulation::ecs::NullEntity));
        }
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
    }
} // namespace lux::editor::ui
