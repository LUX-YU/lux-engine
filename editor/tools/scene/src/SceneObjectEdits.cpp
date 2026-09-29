#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/scene/detail/SceneEditorImpl.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <algorithm>
#include <exception>
#include <cstdio>
#include <lux/engine/editor/scene/detail/EditorEntity.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <unordered_set>

namespace lux::editor::scene
{
    using detail::EditorEntity;
    using detail::kSceneHistoryLimits;
    using detail::ObjectComponent;
    using detail::ObjectContent;
    using detail::SceneContent;
    namespace
    {
        static auto structureFailure(ESceneStructureError code, std::string_view message)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(code),
                message
            ));
        }

        static auto residencyFailure(
            const lux::scene::WorldResidencyFailure& failure,
            std::span<const ObjectContent> objects
        )
        {
            using Error = lux::scene::EWorldResidencyError;
            switch (failure.code)
            {
            case Error::CAPACITY:
                return structureFailure(ESceneStructureError::CAPACITY, "Resident content capacity exceeded");
            case Error::INVALID_PARTITION:
            case Error::NOT_RESIDENT:
                return structureFailure(ESceneStructureError::INVALID_PARTITION, "The partition is not resident");
            case Error::INVALID_OBJECT:
                return structureFailure(ESceneStructureError::INVALID_OBJECT, "The object identity changed");
            case Error::REFERENCE_IN_USE:
                return structureFailure(ESceneStructureError::REFERENCE_IN_USE, "The object is still referenced");
            case Error::MATERIALIZE_FAILURE:
                break;
            }
            const auto& cause = failure.cause;
            const bool has_object = cause.object < objects.size();
            const bool has_component = has_object && cause.data < objects[cause.object].components.size();
            if (cause.code == lux::scene::EWorldMaterializeError::COMPONENT_DECODE_FAILURE && has_component)
            {
                std::array<char, 256> message{};
                std::snprintf(
                    message.data(),
                    message.size(),
                    "%s: decode error %u at byte %zu",
                    objects[cause.object].components[cause.data].schema->id.name.c_str(),
                    static_cast<unsigned>(cause.component.code),
                    cause.component.offset
                );
                return structureFailure(ESceneStructureError::CODEC_FAILURE, message.data());
            }
            return structureFailure(ESceneStructureError::CODEC_FAILURE, "Cannot prepare resident objects");
        }

    } // namespace

    class SceneEditor::Impl::ObjectEdit final : public editing::EditOperation
    {
        using Id = lux::world::WorldObjectId;
        using Members = std::unordered_set<Id, lux::world::WorldObjectIdHash>;
        class Plan final : public editing::PreparedEdit
        {
        public:
            Plan(const ObjectEdit& edit, const editing::ApplyContext& context)
                : edit_(edit), insert_(edit.creation_ == (context.direction == editing::EDirection::FORWARD)),
                  selection_(edit.data_.selection_), revision_(context.next_revision)
            {}
            editing::EditResult<void> prepare(editing::EditPreparationBudget& budget)
            {
                namespace ecs = lux::simulation::ecs;
                auto& owner = edit_.owner_;
                const auto& registry = owner.registry();
                const auto partitions = owner.residency().statistics().resident_partitions;
                const auto pin_charge = budget.reserve(
                    partitions * (sizeof(lux::scene::PartitionRetention) + sizeof(lux::partition::PartitionOrdinal))
                );
                if (!pin_charge)
                {
                    return pin_charge;
                }
                protections_.reserve(partitions);
                protected_partitions_.reserve(partitions);
                Members members;
                members.reserve(edit_.objects_.size());
                for (const auto& object : edit_.objects_)
                {
                    if (!owner.residency().source(object.partition))
                    {
                        return structureFailure(
                            ESceneStructureError::INVALID_OBJECT,
                            "The object's partition is not resident"
                        );
                    }
                    const auto entity = owner.identities().entity(object.object);
                    if (!members.insert(object.object).second ||
                        (insert_ ? entity != ecs::NullEntity : entity == ecs::NullEntity))
                    {
                        return structureFailure(ESceneStructureError::INVALID_OBJECT, "The object identity changed");
                    }
                }
                if (insert_)
                {
                    std::vector<std::vector<lux::scene::WorldComponentInput>> components(edit_.objects_.size());
                    std::vector<lux::scene::WorldResidency::ObjectInput> objects;
                    objects.reserve(edit_.objects_.size());
                    for (std::size_t index{}; index < edit_.objects_.size(); ++index)
                    {
                        const auto& object = edit_.objects_[index];
                        for (const auto& component : object.components)
                            components[index].push_back({component.schema, component.schema->version, component.bytes});
                        objects.push_back({object.partition, {object.object, components[index]}});
                    }
                    auto prepared = owner.residency().prepareCreate(owner.registry(), objects);
                    if (!prepared)
                        return residencyFailure(prepared.error(), edit_.objects_);
                    const auto charged = budget.reserve(prepared->retainedBytes());
                    if (!charged)
                        return charged;
                    change_.emplace(std::move(*prepared));
                    for (const auto entity : change_->references())
                        retainReference(entity);
                }
                else
                {
                    for (const auto protected_entity : registry.view<const EditorEntity>())
                    {
                        if (registry.get<EditorEntity>(protected_entity).deletable)
                        {
                            continue;
                        }
                        auto ancestor = protected_entity;
                        std::size_t visited{};
                        while (registry.valid(ancestor))
                        {
                            if (members.contains(owner.identities().object(ancestor)))
                            {
                                return structureFailure(
                                    ESceneStructureError::INVALID_OBJECT,
                                    "The subtree contains a protected editor entity"
                                );
                            }
                            const auto* parent = registry.try_get<ecs::Parent>(ancestor);
                            if (!parent || ++visited > registry.storage<ecs::Entity>()->size() + 1)
                            {
                                break;
                            }
                            ancestor = parent->entity;
                        }
                    }
                    // Unknown payloads can contain references. Only a provider can
                    // establish their safety; silently deleting around them would corrupt
                    // the preserved author source.
                    bool removes_source_object{};
                    for (const auto& partition : owner.source.partitions)
                    {
                        for (std::size_t index{}; index < partition->objectCount(); ++index)
                        {
                            removes_source_object |= members.contains(partition->objectAt(index).id());
                        }
                    }
                    if (removes_source_object)
                    {
                        for (const auto& schema : owner.source.world->data().schemas())
                        {
                            const auto found = std::ranges::find(
                                owner.metadata.all(),
                                schema.name,
                                [](const auto& value) -> const std::string& { return value.id.name; }
                            );
                            if (found == owner.metadata.all().end())
                            {
                                return structureFailure(ESceneStructureError::MISSING_PROVIDER, schema.name);
                            }
                        }
                    }
                    std::vector<lux::world::WorldObjectId> removed;
                    removed.reserve(edit_.objects_.size());
                    for (const auto& object : edit_.objects_)
                        removed.push_back(object.object);
                    auto prepared = owner.residency().prepareErase(owner.registry(), removed);
                    if (!prepared)
                    {
                        return residencyFailure(prepared.error(), edit_.objects_);
                    }
                    const auto charged = budget.reserve(prepared->retainedBytes());
                    if (!charged)
                        return charged;
                    change_.emplace(std::move(*prepared));
                }
                auto selected = owner.persistent(selection_.object);
                if (insert_)
                {
                    selected = edit_.creation_ ? edit_.objects_.front().object : edit_.selection_before_;
                }
                else if (edit_.creation_)
                {
                    selected = edit_.selection_before_;
                }
                else if (members.contains(selected))
                {
                    selected = {};
                }
                selection_.object = change_->entity(selected);
                if (selection_.revision == UINT64_MAX)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
                }
                ++selection_.revision;
                std::size_t component_count{};
                if (insert_)
                    for (const auto& object : edit_.objects_)
                        component_count += object.components.size();
                return edit_.data_.scene_editing->prepareComponentChanges(component_count);
            }
            editing::EEditEffect effect() const noexcept override
            {
                return editing::EEditEffect::CHANGE;
            }

        private:
            void retainReference(lux::simulation::ecs::Entity entity)
            {
                lux::partition::PartitionOrdinal partition;
                if (!edit_.owner_.residency().partitionOf(entity, partition) ||
                    std::ranges::find(edit_.protected_partitions_, partition) != edit_.protected_partitions_.end() ||
                    std::ranges::find(protected_partitions_, partition) != protected_partitions_.end())
                {
                    return;
                }
                auto retained = edit_.owner_.residency().retain(partition);
                if (!retained)
                {
                    std::terminate();
                }
                protected_partitions_.push_back(partition);
                protections_.push_back(std::move(*retained));
            }
            void apply() noexcept override
            {
                auto& owner = edit_.owner_;
                auto& registry = owner.registry();
                EditingGuard committing(owner.structural_commit);
                for (std::size_t index{}; index < protections_.size(); ++index)
                {
                    edit_.protected_partitions_.push_back(protected_partitions_[index]);
                    edit_.protections_.push_back(std::move(protections_[index]));
                }
                if (!insert_)
                    for (const auto& object : edit_.objects_)
                        edit_.data_.scene_editing->forgetComponentChanges(owner.identities().entity(object.object));
                change_->commit();
                if (insert_)
                    for (const auto& object : edit_.objects_)
                        for (const auto& component : object.components)
                            edit_.data_.scene_editing->recordComponentChange(
                                owner.identities().entity(object.object),
                                component.schema->cpp_type,
                                revision_
                            );
                edit_.data_.selection_ = selection_;
                edit_.data_.structure_revision = revision_;
                owner.structure_changed = true;
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                lux::editor::detail::reportSignalDelivery(
                    edit_.editor_.emit(edit_.editor_.objectsChanged, info.revision),
                    "SceneEditor::objectsChanged"
                );
                lux::editor::detail::reportSignalDelivery(
                    edit_.editor_.emit(edit_.editor_.selectionChanged, edit_.data_.selection_),
                    "SceneEditor::selectionChanged"
                );
            }
            const ObjectEdit& edit_;
            bool insert_;
            std::optional<lux::scene::WorldResidency::PreparedChange> change_;
            std::vector<lux::partition::PartitionOrdinal> protected_partitions_;
            std::vector<lux::scene::PartitionRetention> protections_;
            SelectionNotice selection_;
            editing::Revision revision_;
        };

    public:
        ObjectEdit(
            SceneEditor& editor,
            Impl& data,
            editing::StateId base,
            std::vector<ObjectContent> objects,
            bool creation,
            std::string label
        )
            : editor_(editor), data_(data), owner_(*data.content), base_(base), objects_(std::move(objects)),
              creation_(creation), label_(std::move(label)),
              selection_before_(owner_.persistent(data.selection_.object))
        {
            std::vector<lux::partition::PartitionOrdinal> partitions;
            // Any referenced partition is already resident. Reserve once
            // before EditHistory asks for this operation's retained charge.
            const auto capacity = owner_.residency().statistics().resident_partitions;
            protections_.reserve(capacity);
            protected_partitions_.reserve(capacity);
            const auto add = [&](lux::partition::PartitionOrdinal partition) {
                if (std::ranges::find(partitions, partition) == partitions.end())
                {
                    partitions.push_back(partition);
                }
            };
            for (const auto& object : objects_)
            {
                add(object.partition);
                const auto entity = owner_.identities().entity(object.object);
                if (owner_.registry().valid(entity))
                {
                    std::vector<lux::simulation::ecs::Entity> references;
                    for (const auto& component : object.components)
                    {
                        if (component.schema->visit_references)
                        {
                            static_cast<void>(component.schema->visit_references(
                                owner_.registry(),
                                entity,
                                {&references,
                                 +[](void* state, auto referenced) noexcept {
                                     static_cast<std::vector<lux::simulation::ecs::Entity>*>(state)->push_back(
                                         referenced
                                     );
                                 }}
                            ));
                        }
                    }
                    for (const auto reference : references)
                    {
                        lux::partition::PartitionOrdinal partition;
                        if (owner_.residency().partitionOf(reference, partition))
                        {
                            add(partition);
                        }
                    }
                }
            }
            for (const auto partition : partitions)
            {
                auto retained = owner_.residency().retain(partition);
                if (retained)
                {
                    protected_partitions_.push_back(partition);
                    protections_.push_back(std::move(*retained));
                }
            }
        }
        editing::HistoryId historyId() const noexcept override
        {
            return base_.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return base_;
        }
        std::string_view label() const noexcept override
        {
            return label_;
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            std::size_t bytes = sizeof(*this) + label_.capacity() + 1 + objects_.capacity() * sizeof(ObjectContent) +
                                protections_.capacity() * sizeof(lux::scene::PartitionRetention);
            bytes += protected_partitions_.capacity() * sizeof(lux::partition::PartitionOrdinal);
            for (const auto& object : objects_)
            {
                bytes += object.components.capacity() * sizeof(ObjectComponent);
                for (const auto& component : object.components)
                {
                    bytes += component.bytes.capacity();
                }
            }
            return bytes;
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            // Explicit accounted storage, not a promise about allocator buckets or nested provider heaps.
            const auto charge = [&](std::size_t count, std::size_t bytes) -> editing::EditResult<void> {
                if (bytes && count > budget.remaining() / bytes)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
                }
                return budget.reserve(count * bytes);
            };
            if (!charge(1, sizeof(Plan)))
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
            using IdentityPair = std::pair<lux::world::WorldObjectId, lux::simulation::ecs::Entity>;
            if (!charge(owner_.identities().size(), 2 * sizeof(IdentityPair)) ||
                !charge(objects_.size(), 2 * sizeof(IdentityPair) + sizeof(lux::simulation::ecs::Entity)))
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
            }
            for (const auto& object : objects_)
            {
                if (!charge(
                        object.components.size(),
                        sizeof(ComponentNotice) + sizeof(lux::simulation::ecs::DecodedComponent) +
                            sizeof(lux::simulation::ecs::Entity)
                    ))
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
                }
                for (const auto& component : object.components)
                {
                    if (!charge(1, component.bytes.size()))
                    {
                        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
                    }
                }
            }
            auto result = std::make_unique<Plan>(*this, context);
            auto ready = result->prepare(budget);
            if (!ready)
            {
                return lux::cxx::unexpected(ready.error());
            }
            return editing::PreparedEditPtr(std::move(result));
        }

    private:
        SceneEditor& editor_;
        Impl& data_;
        SceneContent& owner_;
        editing::StateId base_;
        std::vector<ObjectContent> objects_;
        mutable std::vector<lux::scene::PartitionRetention> protections_;
        mutable std::vector<lux::partition::PartitionOrdinal> protected_partitions_;
        bool creation_;
        std::string label_;
        Id selection_before_;
    };

    editing::EditOperationPtr SceneEditor::Impl::makeObjectEdit(
        SceneEditor& editor,
        editing::StateId base,
        std::vector<detail::ObjectContent> content,
        bool creation,
        std::string label
    )
    {
        return std::make_unique<ObjectEdit>(editor, *this, base, std::move(content), creation, std::move(label));
    }
} // namespace lux::editor::scene

namespace lux::editor::scene
{
    class SceneEditor::Impl::ParentEdit final : public editing::EditOperation
    {
        class Plan final : public editing::PreparedEdit
        {
        public:
            Plan(const ParentEdit& edit, bool forward, editing::Revision revision)
                : edit_(edit), forward_(forward), revision_(revision)
            {}
            editing::EEditEffect effect() const noexcept override
            {
                return edit_.before_ == edit_.after_ ? editing::EEditEffect::NO_CHANGE : editing::EEditEffect::CHANGE;
            }

        private:
            void apply() noexcept override
            {
                namespace ecs = lux::simulation::ecs;
                auto& owner = edit_.owner_;
                EditingGuard committing(owner.content->structural_commit);
                const auto parent = forward_ ? edit_.after_ : edit_.before_;
                const auto entity = owner.content->identities().entity(edit_.object_);
                if (forward_ || edit_.had_parent_)
                {
                    owner.registry(owner.scene->id())
                        .emplace_or_replace<ecs::Parent>(entity, owner.content->identities().entity(parent));
                }
                else
                {
                    owner.registry(owner.scene->id()).remove<ecs::Parent>(entity);
                }
                const auto ref = owner.content->identities().entity(edit_.object_);
                const auto type = lux::cxx::typeToken<ecs::Parent>();
                if (forward_ || edit_.had_parent_)
                    owner.scene_editing->recordComponentChange(ref, type, revision_);
                else
                    owner.scene_editing->forgetComponentChange(ref, type);
                lux::partition::PartitionOrdinal partition;
                if (owner.content->residency().partitionOf(entity, partition))
                {
                    static_cast<void>(owner.content->residency().setDirty(partition, true));
                }
                owner.structure_revision = revision_;
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                lux::editor::detail::reportSignalDelivery(
                    edit_.editor_.emit(edit_.editor_.objectsChanged, info.revision),
                    "SceneEditor::objectsChanged"
                );
            }
            const ParentEdit& edit_;
            bool forward_;
            editing::Revision revision_;
        };

    public:
        ParentEdit(SceneEditor& editor, Impl& owner, SceneWriteTarget target, lux::world::WorldObjectId parent)
            : editor_(editor), owner_(owner), target_(target), object_(owner.content->persistent(target.entity)),
              after_(parent)
        {
            const auto* value = owner.registry(owner.scene->id())
                                    .try_get<lux::simulation::ecs::Parent>(owner.content->resolve(target.entity));
            had_parent_ = value != nullptr;
            before_ = value ? owner.content->identities().object(value->entity) : lux::world::WorldObjectId{};
            std::vector<lux::partition::PartitionOrdinal> partitions;
            for (const auto object : {object_, before_, after_})
            {
                lux::partition::PartitionOrdinal partition;
                if (owner.content->residency().partitionOf(owner.content->identities().entity(object), partition) &&
                    std::ranges::find(partitions, partition) == partitions.end())
                {
                    auto retained = owner.content->residency().retain(partition);
                    if (!retained)
                    {
                        std::terminate();
                    }
                    protections_.push_back(std::move(*retained));
                    partitions.push_back(partition);
                }
            }
        }
        editing::HistoryId historyId() const noexcept override
        {
            return target_.state.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return target_.state;
        }
        std::string_view label() const noexcept override
        {
            return "Reparent object";
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return sizeof(*this) + protections_.capacity() * sizeof(lux::scene::PartitionRetention);
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            namespace ecs = lux::simulation::ecs;
            auto charged = budget.reserve(sizeof(Plan));
            if (!charged)
            {
                return lux::cxx::unexpected(charged.error());
            }
            const bool forward = context.direction == editing::EDirection::FORWARD;
            const auto expected = forward ? before_ : after_;
            auto parent = forward ? after_ : before_;
            const auto entity = owner_.content->identities().entity(object_);
            if (entity == ecs::NullEntity ||
                (parent.valid() && owner_.content->identities().entity(parent) == ecs::NullEntity))
            {
                return structureFailure(ESceneStructureError::INVALID_OBJECT, "The object or parent no longer exists");
            }
            const auto* value = owner_.registry(owner_.scene->id()).try_get<ecs::Parent>(entity);
            const auto actual =
                value ? owner_.content->identities().object(value->entity) : lux::world::WorldObjectId{};
            if (actual != expected || (value != nullptr) != (forward ? had_parent_ : true))
            {
                return structureFailure(ESceneStructureError::INVALID_OBJECT, "The object's parent changed");
            }
            if (createsParentCycle(object_, parent, owner_.content->identities().size(), [&](auto id) {
                    const auto* ancestor = owner_.registry(owner_.scene->id())
                                               .try_get<ecs::Parent>(owner_.content->identities().entity(id));
                    return ancestor ? owner_.content->identities().object(ancestor->entity)
                                    : lux::world::WorldObjectId{};
                }))
            {
                return structureFailure(ESceneStructureError::HIERARCHY_CYCLE, "The proposed parent creates a cycle");
            }
            if (auto prepared = owner_.scene_editing->prepareComponentChanges(1); !prepared)
                return lux::cxx::unexpected(prepared.error());
            return editing::PreparedEditPtr(new Plan(*this, forward, context.next_revision));
        }

    private:
        SceneEditor& editor_;
        Impl& owner_;
        SceneWriteTarget target_;
        lux::world::WorldObjectId object_, before_, after_;
        std::vector<lux::scene::PartitionRetention> protections_;
        bool had_parent_;
    };

    editing::EditOperationPtr SceneEditor::Impl::makeParentEdit(
        SceneEditor& editor,
        SceneWriteTarget target,
        lux::world::WorldObjectId parent
    )
    {
        return std::make_unique<ParentEdit>(editor, *this, target, parent);
    }
}
