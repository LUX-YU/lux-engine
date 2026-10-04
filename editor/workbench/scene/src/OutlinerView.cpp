#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <lux/engine/editor/widgets/TreeRows.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/scene/RenderViewRequest.hpp>
#include <lux/engine/ui/Element.hpp>
#include <imgui.h>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <cstring>
#include <type_traits>
namespace lux::editor::scene
{
    namespace
    {
        template <class T> auto rejected(T error)
        {
            return cxx::unexpected(SceneViewFailure{std::move(error)});
        }
        bool busy(const SceneViewResult<void>& result)
        {
            if (result)
                return false;
            const auto* error = std::get_if<SceneEditError>(&result.error().cause);
            return error && (error->code == ESceneEditError::BUSY || (error->code == ESceneEditError::SESSION &&
                                                                      error->session == sessions::ESessionError::BUSY));
        }
        struct SelectionHash final
        {
            std::size_t operator()(const VSceneSelectionTarget& target) const noexcept
            {
                std::size_t result = target.index();
                const auto append = [&](std::uint64_t value) {
                    result ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b9 + (result << 6) + (result >> 2);
                };
                std::visit([&](const auto& value) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, SceneObjectRef>)
                    {
                        append(value.session.domain);
                        append(value.session.slot);
                        append(value.session.generation);
                        append(value.history.value);
                        append(world::WorldObjectIdHash{}(value.object));
                    }
                    else
                    {
                        append(value.run.domain);
                        append(value.run.slot);
                        append(value.run.generation);
                        append(lux::scene::SceneInstanceId::Hash{}(value.instance));
                        append(simulation::ecs::entityBits(value.entity));
                    }
                }, target);
                return result;
            }
        };
        constexpr const char* objectPayload = "lux.editor.scene.author-object.v4";
        SceneInteractionGroup* group(const VSceneViewBinding& binding) noexcept
        {
            return std::visit(
                [](const auto& value) -> SceneInteractionGroup* {
                    if constexpr (requires { value.interaction; })
                        return value.interaction;
                    return nullptr;
                },
                binding
            );
        }
    }
    struct OutlinerView::Impl final
    {
        std::shared_ptr<SceneInteractionGroup> interaction_;
        struct Rows final
        {
            std::vector<VSceneSelectionTarget> objects;
            std::vector<std::string> labels;
            std::vector<widgets::TreeRow> tree;
            std::optional<sessions::ContentStamp> content;
            lux::scene::SceneInstanceId instance;
            std::uint64_t structure{};
            std::array<bool, 3> can_create{};
            bool can_reparent{};
        };
        class Content final : public lux::ui::Element
        {
        public:
            Content(OutlinerView& view, Impl& owner) : Element(view, lux::ui::ElementId{"objects"}), owner_(owner) {}

        private:
            void draw() noexcept override
            {
                owner_.draw();
            }
            Impl& owner_;
        };
        OutlinerView& view_;
        sessions::TSessionAccess<SceneSession> sessions_;
        simulation::ecs::ComponentSchemaSet schemas_;
        std::optional<RunInspectAccess> runs_;
        VSceneViewBinding binding_{UnboundSceneBinding{}};
        Rows rows_;
        std::optional<VSceneSelectionTarget> selection_request_;
        std::optional<SceneObjectRef> erase_request_;
        std::optional<std::pair<SceneObjectRef, world::WorldObjectId>> parent_request_;
        std::optional<EObjectSpace> create_request_;
        std::uint32_t partition_{};
        std::unordered_set<VSceneSelectionTarget, SelectionHash> collapsed_;
        std::vector<std::size_t> visible_;
        SceneViewResult<void> status_;
        Content content_;
        Impl(
            OutlinerView& view,
            sessions::TSessionAccess<SceneSession> sessions,
            std::optional<RunInspectAccess> runs,
            simulation::ecs::ComponentSchemaSet schemas
        )
            : view_(view), sessions_(sessions), schemas_(std::move(schemas)), runs_(runs), content_(view, *this)
        {
            view.setContent(content_);
        }
        void visibleRows()
        {
            visible_.clear();
            for (std::size_t i{}; i < rows_.tree.size();)
            {
                visible_.push_back(i);
                const auto& row = rows_.tree[i];
                i = collapsed_.contains(rows_.objects[row.source]) ? row.end : i + 1;
            }
        }

        SceneViewResult<Rows> readRows(const VSceneViewBinding& binding)
        {
            Rows result;
            std::vector<std::size_t> parents;
            if (auto* author = std::get_if<EditedSceneBinding>(&binding))
            {
                if (!author->interaction || author->interaction->session() != author->session)
                    return rejected(views::EViewError::INVALID_ID);
                auto session = sessions_.read(author->session);
                if (!session)
                    return rejected(SceneEditError{session.error()});
                auto read = session->get().read();
                if (!read)
                    return rejected(read.error());
                result.content = session->get().describe().current;
                auto available = read->withRead([&](const SceneReadView& source) -> SceneEditResult<void> {
                    const auto facts = source.facts();
                    const std::string_view transform2d[]{"lux.ecs.Transform2D"};
                    const std::string_view transform3d[]{"lux.ecs.Transform3D"};
                    const std::string_view parent[]{"lux.ecs.Parent"};
                    result.can_create = {
                        queryApplicability(facts, {{}, false, true}).supported(),
                        queryApplicability(facts, {transform2d, false, true}).supported(),
                        queryApplicability(facts, {transform3d, false, true}).supported()
                    };
                    result.can_reparent = queryApplicability(facts, {parent, false, true}).supported();
                    return {};
                });
                if (!available)
                    return rejected(available.error());
                const auto objects = read->objects();
                std::unordered_map<world::WorldObjectId, std::size_t, world::WorldObjectIdHash> indices;
                for (std::size_t i{}; i < objects.size(); ++i)
                {
                    result.objects.emplace_back(objects[i]);
                    result.labels.push_back(uuids::to_string(objects[i].object.value));
                    indices.emplace(objects[i].object, i);
                }
                parents.assign(objects.size(), objects.size());
                for (std::size_t i{}; i < objects.size(); ++i)
                {
                    auto parent = read->parent(objects[i]);
                    if (!parent)
                        return rejected(parent.error());
                    if (auto found = indices.find(*parent); found != indices.end())
                        parents[i] = found->second;
                }
            }
            else if (auto* running = std::get_if<RunningSceneBinding>(&binding))
            {
                if (!runs_ || !running->interaction)
                    return rejected(views::EViewError::INVALID_ID);
                auto info = runs_->describe(running->run);
                if (!info)
                    return rejected(info.error());
                result.instance = info->instance;
                result.structure = info->structure_revision;
                const bool wrong_group = running->interaction->run() ? *running->interaction->run() != running->run
                                                                     : !running->interaction->session() ||
                                                                           info->provenance.content.session !=
                                                                               running->interaction->session()->id();
                if (wrong_group)
                    return rejected(views::EViewError::INVALID_ID);
                auto read = runs_->borrow(running->run);
                if (!read)
                    return rejected(read.error());
                const auto& registry = read->get();
                std::unordered_map<simulation::ecs::Entity, std::size_t> indices;
                for (auto [entity] : registry.storage<simulation::ecs::Entity>()->each())
                {
                    auto* request = registry.try_get<lux::scene::RenderViewRequest>(entity);
                    if (request && request->destroy_entity_on_stop)
                        continue;
                    auto ref = runs_->reference(running->run, entity);
                    if (!ref)
                        return rejected(ref.error());
                    indices.emplace(entity, result.objects.size());
                    result.objects.emplace_back(*ref);
                    result.labels.push_back("Object " + std::to_string(simulation::ecs::entityBits(entity)));
                }
                parents.assign(result.objects.size(), result.objects.size());
                for (std::size_t i{}; i < result.objects.size(); ++i)
                    if (auto* parent = registry.try_get<simulation::ecs::Parent>(
                            std::get<RunningObjectRef>(result.objects[i]).entity
                        ))
                        if (auto found = indices.find(parent->entity); found != indices.end())
                            parents[i] = found->second;
            }
            result.tree = widgets::makeTreeRows(parents);
            return result;
        }
        SceneViewResult<void> rebind(VSceneViewBinding binding)
        {
            if (object::LuxObject::isDispatching())
                return rejected(views::EViewError::BUSY);
            if (interaction_ && group(binding) != interaction_.get())
                return rejected(views::EViewError::INVALID_ID);
            auto candidate = readRows(binding);
            if (!candidate)
                return rejected(candidate.error());
            if (binding != binding_)
                if (auto* old = group(binding_))
                {
                    auto cancelled = old->cancel();
                    if (!cancelled)
                        return rejected(cancelled.error());
                }
            binding_ = std::move(binding);
            rows_ = std::move(*candidate);
            collapsed_.clear();
            visibleRows();
            selection_request_.reset();
            erase_request_.reset();
            parent_request_.reset();
            create_request_.reset();
            return {};
        }
        SceneViewResult<void> select(VSceneSelectionTarget target)
        {
            auto* interaction = group(binding_);
            if (!interaction)
                return rejected(views::EViewError::NOT_ATTACHED);
            const bool is_author = std::holds_alternative<EditedSceneBinding>(binding_);
            if (is_author != std::holds_alternative<SceneObjectRef>(target))
                return rejected(views::EViewError::INVALID_ID);
            if (auto* running = std::get_if<RunningSceneBinding>(&binding_);
                running && std::get<RunningObjectRef>(target).run != running->run)
                return rejected(views::EViewError::INVALID_ID);
            auto current = readRows(binding_);
            if (!current)
                return rejected(current.error());
            if (std::ranges::find(current->objects, target) == current->objects.end())
                return rejected(views::EViewError::INVALID_ID);
            auto cancelled = interaction->cancel();
            if (!cancelled)
                return rejected(cancelled.error());
            auto selected = interaction->select({{std::move(target)}});
            if (!selected)
                return rejected(selected.error());
            return {};
        }
        SceneViewResult<void> apply(std::vector<VSceneEdit> edits, const char* label)
        {
            auto* author = std::get_if<EditedSceneBinding>(&binding_);
            if (!author)
                return rejected(views::EViewError::NOT_ATTACHED);
            if (author->interaction->overlay())
                return rejected(views::EViewError::BUSY);
            auto session = sessions_.edit(author->session);
            if (!session)
                return rejected(SceneEditError{session.error()});
            auto applied = session->get().apply({session->get().describe().current, label, std::move(edits)});
            if (!applied)
                return rejected(applied.error());
            return {};
        }
        void update() noexcept
        {
            if (selection_request_)
            {
                status_ = select(*selection_request_);
                if (!busy(status_))
                    selection_request_.reset();
            }
            if (erase_request_)
            {
                status_ = view_.erase(std::span(&*erase_request_, 1));
                if (!busy(status_))
                    erase_request_.reset();
            }
            if (parent_request_)
            {
                status_ = view_.reparent(parent_request_->first, parent_request_->second);
                if (!busy(status_))
                    parent_request_.reset();
            }
            if (create_request_)
            {
                std::random_device seed;
                std::mt19937 random(seed());
                status_ = view_.createObject({uuids::uuid_random_generator(random)()}, {partition_}, *create_request_);
                if (!busy(status_))
                    create_request_.reset();
            }
            if (auto* author = std::get_if<EditedSceneBinding>(&binding_))
            {
                auto info = sessions_.describe(author->session);
                if (!info)
                {
                    status_ = rejected(SceneEditError{info.error()});
                    return;
                }
                if (rows_.content == info->current)
                    return;
            }
            else if (auto* running = std::get_if<RunningSceneBinding>(&binding_); running && runs_)
            {
                auto info = runs_->describe(running->run);
                if (!info)
                {
                    status_ = rejected(info.error());
                    return;
                }
                if (rows_.instance == info->instance && rows_.structure == info->structure_revision)
                    return;
            }
            else
                return;
            auto candidate = readRows(binding_);
            if (candidate)
            {
                rows_ = std::move(*candidate);
                // One current identity set, then one lookup per collapsed item. Labels are presentation only.
                const std::unordered_set<VSceneSelectionTarget, SelectionHash> live(
                    rows_.objects.begin(), rows_.objects.end()
                );
                std::erase_if(collapsed_, [&](const auto& target) { return !live.contains(target); });
                visibleRows();
            }
            else
                status_ = rejected(candidate.error());
        }
        void draw() noexcept
        {
            if (!status_)
                ImGui::TextUnformatted("Operation unavailable; previous objects retained");
            const bool author = std::holds_alternative<EditedSceneBinding>(binding_);
            ImGui::BeginDisabled(!author);
            if (ImGui::SmallButton("Create"))
                ImGui::OpenPopup("create-object");
            if (ImGui::BeginPopup("create-object"))
            {
                ImGui::InputScalar("Partition", ImGuiDataType_U32, &partition_);
                if (ImGui::MenuItem("Empty object", nullptr, false, rows_.can_create[0]))
                    create_request_ = EObjectSpace::NONE;
                if (ImGui::MenuItem(
                        "2D object",
                        nullptr,
                        false,
                        rows_.can_create[1]
                    ))
                    create_request_ = EObjectSpace::SPACE_2D;
                if (ImGui::MenuItem(
                        "3D object",
                        nullptr,
                        false,
                        rows_.can_create[2]
                    ))
                    create_request_ = EObjectSpace::SPACE_3D;
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
            bool visibility_changed{};
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible_.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto row_index = visible_[i];
                    const auto& row = rows_.tree[row_index];
                    const auto& target = rows_.objects[row.source];
                    ImGui::PushID(static_cast<int>(row.source));
                    const auto indent = static_cast<float>(row.depth) * ImGui::GetStyle().IndentSpacing;
                    ImGui::Indent(indent);
                    auto* interaction = group(binding_);
                    const bool selected = interaction && std::ranges::find(interaction->selection().objects, target) !=
                                                             interaction->selection().objects.end();
                    const bool has_children = row.end > row_index + 1;
                    ImGui::SetNextItemOpen(!collapsed_.contains(rows_.objects[row.source]), ImGuiCond_Always);
                    const auto flags = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_OpenOnArrow |
                                       ImGuiTreeNodeFlags_SpanAvailWidth |
                                       (selected ? ImGuiTreeNodeFlags_Selected : 0) |
                                       (has_children ? 0 : ImGuiTreeNodeFlags_Leaf);
                    const bool open = ImGui::TreeNodeEx("object", flags, "%s", rows_.labels[row.source].c_str());
                    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                        selection_request_ = target;
                    if (has_children && open == collapsed_.contains(rows_.objects[row.source]))
                    {
                        if (open)
                            collapsed_.erase(target);
                        else
                            collapsed_.insert(target);
                        visibility_changed = true;
                    }
                    if (auto* object = std::get_if<SceneObjectRef>(&target))
                    {
                        if (ImGui::BeginDragDropSource())
                        {
                            ImGui::SetDragDropPayload(objectPayload, object, sizeof(*object));
                            ImGui::TextUnformatted(rows_.labels[row.source].c_str());
                            ImGui::EndDragDropSource();
                        }
                        if (rows_.can_reparent && ImGui::BeginDragDropTarget())
                        {
                            if (auto* payload = ImGui::AcceptDragDropPayload(objectPayload);
                                payload && payload->DataSize == sizeof(SceneObjectRef))
                            {
                                SceneObjectRef source;
                                std::memcpy(&source, payload->Data, sizeof(source));
                                if (source.session == object->session && source.history == object->history)
                                    parent_request_ = {source, object->object};
                            }
                            ImGui::EndDragDropTarget();
                        }
                    }
                    if (auto* author = std::get_if<SceneObjectRef>(&target); author && ImGui::BeginPopupContextItem())
                    {
                        if (ImGui::MenuItem("Delete"))
                            erase_request_ = *author;
                        if (ImGui::MenuItem("Make root", nullptr, false, rows_.can_reparent))
                            parent_request_ = {*author, {}};
                        ImGui::EndPopup();
                    }
                    ImGui::Unindent(indent);
                    ImGui::PopID();
                }
            if (visibility_changed)
                visibleRows();
        }
    };
    OutlinerView::OutlinerView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        sessions::TSessionAccess<SceneSession> sessions,
        VSceneViewBinding binding,
        std::optional<RunInspectAccess> runs,
        simulation::ecs::ComponentSchemaSet schemas,
        std::shared_ptr<SceneInteractionGroup> interaction
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.outliner"}, "Outliner"),
          impl_(std::make_unique<Impl>(*this, sessions, runs, std::move(schemas)))
    {
        impl_->interaction_ = std::move(interaction);
        impl_->status_ = impl_->rebind(std::move(binding));
    }
    const std::shared_ptr<SceneInteractionGroup>& OutlinerView::interactionOwner() const noexcept
    {
        return impl_->interaction_;
    }
    OutlinerView::~OutlinerView() noexcept = default;
    SceneViewResult<void> OutlinerView::rebind(VSceneViewBinding value)
    {
        return impl_->rebind(std::move(value));
    }
    SceneViewResult<void> OutlinerView::select(VSceneSelectionTarget value)
    {
        return impl_->select(std::move(value));
    }
    SceneViewResult<void> OutlinerView::setCollapsed(VSceneSelectionTarget target, bool collapsed)
    {
        if (std::ranges::find(impl_->rows_.objects, target) == impl_->rows_.objects.end())
            return rejected(views::EViewError::INVALID_ID);
        if (collapsed)
            impl_->collapsed_.insert(std::move(target));
        else
            impl_->collapsed_.erase(target);
        impl_->visibleRows();
        return {};
    }
    bool OutlinerView::isCollapsed(const VSceneSelectionTarget& target) const noexcept
    {
        return impl_->collapsed_.contains(target);
    }
    SceneViewResult<void> OutlinerView::erase(std::span<const SceneObjectRef> targets)
    {
        std::vector<VSceneEdit> edits;
        for (auto target : targets)
            edits.push_back(SceneEraseObject{target});
        return impl_->apply(std::move(edits), "Delete objects");
    }
    SceneViewResult<void> OutlinerView::reparent(SceneObjectRef target, world::WorldObjectId parent)
    {
        std::vector<VSceneEdit> edits;
        edits.push_back(SceneReparentObject{target, parent});
        return impl_->apply(std::move(edits), "Reparent object");
    }
    SceneViewResult<void> OutlinerView::createObject(
        world::WorldObjectId id,
        partition::PartitionOrdinal partition,
        EObjectSpace space
    )
    {
        const auto* binding = std::get_if<EditedSceneBinding>(&impl_->binding_);
        if (!binding)
            return rejected(views::EViewError::NOT_ATTACHED);
        auto session = impl_->sessions_.read(binding->session);
        if (!session)
            return rejected(SceneEditError{session.error()});
        auto read = session->get().read();
        if (!read)
            return rejected(read.error());
        auto object = read->withRead([&](const SceneReadView& source) -> SceneEditResult<SceneObjectData> {
            const auto facts = source.facts();
            const std::string_view names[]{space == EObjectSpace::SPACE_2D ? "lux.ecs.Transform2D" : "lux.ecs.Transform3D"};
            const auto required = space == EObjectSpace::NONE ? std::span<const std::string_view>{} : std::span{names};
            const auto allowed = queryApplicability(facts, {required, false, true});
            if (!allowed.supported())
                return cxx::unexpected(SceneEditError{
                    allowed.reason == EApplicabilityReason::INDEX_REBUILD_REQUIRED
                        ? ESceneEditError::INDEX_REBUILD_REQUIRED : ESceneEditError::MISSING_SCHEMA
                });
            const std::string_view parent[]{"lux.ecs.Parent"};
            const bool hierarchy = queryApplicability(facts, {parent}).supported();
            auto encoded = makeSceneObject(id, partition, space, hierarchy, impl_->schemas_);
            if (!encoded)
            {
                SceneEditError failure{ESceneEditError::CODEC};
                failure.history = encoded.error();
                return cxx::unexpected(failure);
            }
            return std::move(*encoded);
        });
        if (!object)
            return rejected(object.error());
        std::vector<VSceneEdit> edits;
        edits.emplace_back(SceneCreateObject{std::move(*object)});
        return impl_->apply(std::move(edits), "Create object");
    }
    views::ViewContent OutlinerView::content() const noexcept
    {
        const auto* author = std::get_if<EditedSceneBinding>(&impl_->binding_);
        return author ? views::ViewContent{{author->session.id()}, author->session.id()} : views::ViewContent{};
    }
    std::span<const VSceneSelectionTarget> OutlinerView::objects() const noexcept
    {
        return impl_->rows_.objects;
    }
    const SceneViewResult<void>& OutlinerView::status() const noexcept
    {
        return impl_->status_;
    }
    void OutlinerView::update() noexcept
    {
        impl_->update();
    }
    SceneViewResult<views::DetachedView> makeOutlinerView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        sessions::TSessionAccess<SceneSession> sessions,
        VSceneViewBinding binding,
        std::optional<RunInspectAccess> runs,
        simulation::ecs::ComponentSchemaSet schemas,
        std::shared_ptr<SceneInteractionGroup> interaction
    )
    {
        auto view = std::make_unique<OutlinerView>(
            dispatcher,
            std::move(id),
            sessions,
            std::move(binding),
            runs,
            std::move(schemas),
            std::move(interaction)
        );
        if (!view->status())
            return cxx::unexpected(view->status().error());
        return views::DetachedView{
            lux::object::CodeLease::builtin(), std::move(view), nullptr, nullptr, nullptr, nullptr,
            +[](const lux::ui::Pane& pane) noexcept { return static_cast<const OutlinerView&>(pane).content(); }
        };
    }
}
