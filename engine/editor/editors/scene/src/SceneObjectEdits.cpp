#include <algorithm>
#include <lux/engine/editor/scene/detail/EditorEntity.hpp>
#include <lux/engine/editor/scene/detail/SceneObjectEdits.hpp>
#include <lux/engine/simulation/ecs/EntityCreationPlan.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <unordered_set>

namespace lux::editor::scene::detail
{
namespace
{
struct EditingGuard final
{
    explicit EditingGuard(bool &value) : value_(value)
    {
        value_ = true;
    }
    ~EditingGuard()
    {
        value_ = false;
    }
    bool &value_;
};
static auto structureFailure(ESceneStructureError code, std::string_view message)
{
    return lux::cxx::unexpected(
        editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED, static_cast<std::uint64_t>(code), message));
}

class SceneObjectEdit final : public editing::EditOperation
{
    using Id = lux::world::WorldObjectId;
    using Members = std::unordered_set<Id, lux::world::WorldObjectIdHash>;
    class Plan final : public editing::PreparedEdit
    {
      public:
        Plan(const SceneObjectEdit &edit, const editing::ApplyContext &context)
            : edit_(edit), insert_(edit.creation_ == (context.direction == editing::EDirection::FORWARD)),
              rows_(edit.owner_.rows), versions_(edit.owner_.component_versions), selection_(edit.owner_.selection),
              revision_(context.next_revision)
        {
        }
        editing::EditResult<void> prepare(editing::EditPreparationBudget &budget)
        {
            namespace ecs = lux::simulation::ecs;
            auto &owner = edit_.owner_;
            const auto &registry = owner.registry;
            const auto partitions = owner.loading.statistics().resident_partitions;
            const auto pin_charge = budget.reserve(
                partitions * (sizeof(lux::scene::PartitionRetention) + sizeof(lux::partition::PartitionOrdinal)));
            if (!pin_charge)
            {
                return pin_charge;
            }
            protections_.reserve(partitions);
            protected_partitions_.reserve(partitions);
            Members members;
            members.reserve(edit_.objects_.size());
            for (const auto &object : edit_.objects_)
            {
                if (!owner.loading.source(object.row.partition))
                {
                    return structureFailure(ESceneStructureError::INVALID_OBJECT,
                                            "The object's partition is not resident");
                }
                const auto entity = owner.identities.entity(object.row.object);
                if (!members.insert(object.row.object).second ||
                    (insert_ ? entity != ecs::NullEntity : entity == ecs::NullEntity))
                {
                    return structureFailure(ESceneStructureError::INVALID_OBJECT, "The object identity changed");
                }
            }
            identities_.reserve(owner.identities.size() + edit_.objects_.size());
            for (const auto &[id, entity] : owner.identities.entries())
            {
                if (insert_ || !members.contains(id))
                {
                    if (!identities_.bind(id, entity))
                    {
                        std::terminate();
                    }
                }
            }
            if (insert_)
            {
                std::size_t component_bytes{};
                for (const auto &object : edit_.objects_)
                {
                    for (const auto &component : object.components)
                    {
                        component_bytes += component.schema->operations.valueBytes();
                    }
                }
                const auto capacity = owner.loading.validateAdditional(edit_.objects_.size(), component_bytes);
                if (!capacity)
                {
                    return structureFailure(ESceneStructureError::CAPACITY,
                                            "Resident entity/component capacity is exhausted");
                }
                auto planned = ecs::planEntityCreation(registry, edit_.objects_.size());
                if (!planned)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
                }
                creation_.emplace<ecs::EntityCreationPlan>(std::move(*planned));
                std::size_t index{};
                for (const auto &object : edit_.objects_)
                {
                    if (!identities_.bind(object.row.object,
                                          std::get<ecs::EntityCreationPlan>(creation_).entities()[index++]))
                    {
                        return structureFailure(ESceneStructureError::INVALID_OBJECT, "Duplicate planned identity");
                    }
                }
                const ecs::ComponentEntityResolver resolver{
                    this,
                    [](const void *state, lux::world::WorldObjectId id) noexcept
                        -> lux::cxx::expected<ecs::Entity, ecs::ComponentDecodeFailure> {
                        auto &plan = *const_cast<Plan *>(static_cast<const Plan *>(state));
                        const auto entity = plan.identities_.entity(id);
                        if (entity == ecs::NullEntity && id.valid())
                        {
                            return lux::cxx::unexpected(
                                ecs::ComponentDecodeFailure{ecs::EComponentDecodeError::UNRESOLVED_REFERENCE, 0, id});
                        }
                        plan.retainReference(entity);
                        return entity;
                    }};
                for (const auto &object : edit_.objects_)
                {
                    const auto entity = identities_.entity(object.row.object);
                    std::vector<lux::cxx::TypeToken> types;
                    types.reserve(object.components.size());
                    for (const auto &component : object.components)
                    {
                        const auto &schema = *component.schema;
                        if (!schema.decode_value || std::ranges::find(types, schema.cpp_type) != types.end())
                        {
                            return structureFailure(ESceneStructureError::MISSING_PROVIDER, schema.id.name);
                        }
                        types.push_back(schema.cpp_type);
                        auto decoded =
                            schema.decode_value(schema.version, component.bytes, resolver, schema.code_lifetime);
                        if (!decoded)
                        {
                            return structureFailure(ESceneStructureError::CODEC_FAILURE,
                                                    schema.id.name + ": decode error " +
                                                        std::to_string(static_cast<unsigned>(decoded.error().code)) +
                                                        " at byte " + std::to_string(decoded.error().offset));
                        }
                        const auto charged = budget.reserve(decoded->accountedBytes());
                        if (!charged)
                        {
                            return charged;
                        }
                        values_.push_back({entity, std::move(*decoded)});
                        versions_.push_back({owner.reference(entity), schema.cpp_type, revision_});
                    }
                    rows_.push_back({owner.reference(entity), owner.reference(identities_.entity(object.row.parent)),
                                     object.row.label, object.row.partition});
                }
                std::ranges::sort(rows_, std::less<SceneEntityRef>{}, &SceneObjectRow::object);
                std::ranges::sort(versions_, SceneObjects::componentLess);
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
                        if (members.contains(owner.identities.object(ancestor)))
                        {
                            return structureFailure(ESceneStructureError::INVALID_OBJECT,
                                                    "The subtree contains a protected editor entity");
                        }
                        const auto *parent = registry.try_get<ecs::Parent>(ancestor);
                        if (!parent || ++visited > owner.rows.size() + 1)
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
                for (const auto &partition : owner.source.partitions)
                {
                    for (std::size_t index{}; index < partition->objectCount(); ++index)
                    {
                        removes_source_object |= members.contains(partition->objectAt(index).id());
                    }
                }
                if (removes_source_object)
                {
                    for (const auto &schema : owner.source.world->data().schemas())
                    {
                        const auto found =
                            std::ranges::find(owner.metadata.components().all(), schema.name,
                                              [](const auto &value) -> const std::string & { return value.id.name; });
                        if (found == owner.metadata.components().all().end())
                        {
                            return structureFailure(ESceneStructureError::MISSING_PROVIDER, schema.name);
                        }
                    }
                }
                // EditHistory validates the exact content StateId. Author writes are private to
                // this owner; replay therefore uses the retained capture. Wire-byte equality is not
                // value equality for unordered containers, and must not reject a valid replay.
                // Encoding with the projected identity index validates every known
                // Entity reference, including nested containers. Only one component's
                // temporary bytes are retained.
                for (const auto &[id, entity] : identities_.entries())
                {
                    for (const auto &schema : owner.metadata.components().all())
                    {
                        if (schema.semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                            !schema.operations.has(registry, entity))
                        {
                            continue;
                        }
                        if (!schema.capture)
                        {
                            return structureFailure(ESceneStructureError::MISSING_PROVIDER, schema.id.name);
                        }
                        auto value = schema.capture(registry, entity, schema.code_lifetime);
                        if (!value)
                        {
                            return structureFailure(ESceneStructureError::CODEC_FAILURE, schema.id.name);
                        }
                        auto original = value->encode(owner.identities, kSceneHistoryLimits.max_staging_bytes);
                        if (!original)
                        {
                            return structureFailure(ESceneStructureError::CODEC_FAILURE, schema.id.name);
                        }
                        auto encoded = value->encode(identities_, kSceneHistoryLimits.max_staging_bytes);
                        if (!encoded)
                        {
                            return structureFailure(ESceneStructureError::REFERENCE_IN_USE, schema.id.name);
                        }
                    }
                }
                std::erase_if(rows_, [&](const auto &row) { return members.contains(owner.persistent(row.object)); });
                std::erase_if(versions_,
                              [&](const auto &value) { return members.contains(owner.persistent(value.object)); });
            }
            auto selected = owner.persistent(selection_.object);
            if (insert_)
            {
                selected = edit_.creation_ ? edit_.objects_.front().row.object : edit_.selection_before_;
            }
            else if (edit_.creation_)
            {
                selected = edit_.selection_before_;
            }
            else if (members.contains(selected))
            {
                selected = {};
            }
            selection_.object = owner.reference(identities_.entity(selected));
            if (selection_.revision == UINT64_MAX || versions_.size() > UINT64_MAX - owner.next_component_change)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
            }
            ++selection_.revision;
            if (insert_ && !ecs::validateEntityCreation(registry, std::get<ecs::EntityCreationPlan>(creation_)))
            {
                return structureFailure(ESceneStructureError::INVALID_OBJECT,
                                        "Registry structure changed during preparation");
            }
            // The edit pins these partitions for its complete history
            // lifetime. Prepare their post-commit membership once.
            for (const auto &object : edit_.objects_)
            {
                const auto ordinal = object.row.partition;
                if (std::ranges::find(memberships_, ordinal, &Membership::partition) != memberships_.end())
                {
                    continue;
                }
                Membership membership{ordinal};
                for (const auto &row : rows_)
                {
                    if (row.partition == ordinal)
                    {
                        membership.entities.push_back(row.object.entity);
                    }
                }
                auto charged =
                    budget.reserve(sizeof(Membership) + membership.entities.capacity() * sizeof(ecs::Entity));
                if (!charged)
                {
                    return charged;
                }
                memberships_.push_back(std::move(membership));
            }
            return {};
        }
        editing::EEditEffect effect() const noexcept override
        {
            return editing::EEditEffect::CHANGE;
        }

      private:
        void retainReference(lux::simulation::ecs::Entity entity)
        {
            lux::partition::PartitionOrdinal partition;
            if (!edit_.owner_.loading.partitionOf(entity, partition) ||
                std::ranges::find(edit_.protected_partitions_, partition) != edit_.protected_partitions_.end() ||
                std::ranges::find(protected_partitions_, partition) != protected_partitions_.end())
            {
                return;
            }
            auto retained = edit_.owner_.loading.retain(partition);
            assert(retained);
            protected_partitions_.push_back(partition);
            protections_.push_back(std::move(*retained));
        }
        void apply() noexcept override
        {
            auto &owner = edit_.owner_;
            auto &registry = owner.registry;
            EditingGuard committing(owner.structural_commit);
            for (std::size_t index{}; index < protections_.size(); ++index)
            {
                edit_.protected_partitions_.push_back(protected_partitions_[index]);
                edit_.protections_.push_back(std::move(protections_[index]));
            }
            if (insert_)
            {
                for (const auto &object : edit_.objects_)
                {
                    const auto entity = identities_.entity(object.row.object);
                    if (registry.create(entity) != entity)
                    {
                        std::terminate(); // Owner exclusion preserves prepared, unused
                                          // EnTT identities.
                    }
                }
                owner.identities = std::move(identities_);
                for (auto &value : values_)
                {
                    std::move(value.component).installInto(registry, value.entity);
                }
            }
            else
            {
                for (auto object = edit_.objects_.rbegin(); object != edit_.objects_.rend(); ++object)
                {
                    // on_destroy observes the old component and its old durable
                    // identity.
                    registry.destroy(owner.identities.entity(object->row.object));
                }
                owner.identities = std::move(identities_);
            }
            for (auto &version : versions_)
            {
                if (version.revision == revision_)
                {
                    version.sequence = owner.next_component_change++;
                }
            }
            owner.rows.swap(rows_);
            for (auto &membership : memberships_)
            {
                const auto committed =
                    owner.loading.replaceEntities(membership.partition, std::move(membership.entities));
                if (!committed)
                {
                    std::terminate(); // Prepared membership, retained partition, exclusive structural owner.
                }
            }
            owner.component_versions.swap(versions_);
            owner.selection = selection_;
            owner.structure_revision = revision_;
            owner.structure_changed = true;
            owner.invalidate_derived = true;
        }
        void publish(const editing::CommitInfo &info) noexcept override
        {
            edit_.editor_.notify<SceneEditor::objectsChanged>(info.revision);
            edit_.editor_.notify<SceneEditor::selectionChanged>(edit_.owner_.selection);
        }
        const SceneObjectEdit &edit_;
        bool insert_;
        struct Value final
        {
            lux::simulation::ecs::Entity entity;
            lux::simulation::ecs::DecodedComponent component;
        };
        std::variant<std::monostate, lux::simulation::ecs::EntityCreationPlan> creation_;
        std::vector<Value> values_;
        struct Membership final
        {
            lux::partition::PartitionOrdinal partition;
            std::vector<lux::simulation::ecs::Entity> entities;
        };
        std::vector<Membership> memberships_;
        std::vector<lux::partition::PartitionOrdinal> protected_partitions_;
        std::vector<lux::scene::PartitionRetention> protections_;
        lux::simulation::ecs::WorldEntityMap identities_;
        std::vector<SceneObjectRow> rows_;
        std::vector<ComponentNotice> versions_;
        SelectionNotice selection_;
        editing::Revision revision_;
    };

  public:
    SceneObjectEdit(SceneEditor &editor, SceneObjects &owner, editing::StateId base, std::vector<ObjectContent> objects,
                    bool creation, std::string label)
        : editor_(editor), owner_(owner), base_(base), objects_(std::move(objects)), creation_(creation),
          label_(std::move(label)), selection_before_(owner.persistent(owner.selection.object))
    {
        std::vector<lux::partition::PartitionOrdinal> partitions;
        // Any referenced partition is already resident. Reserve once
        // before EditHistory asks for this operation's retained charge.
        const auto capacity = owner_.loading.statistics().resident_partitions;
        protections_.reserve(capacity);
        protected_partitions_.reserve(capacity);
        const auto add = [&](lux::partition::PartitionOrdinal partition) {
            if (std::ranges::find(partitions, partition) == partitions.end())
            {
                partitions.push_back(partition);
            }
        };
        for (const auto &object : objects_)
        {
            add(object.row.partition);
            const auto entity = owner_.identities.entity(object.row.object);
            if (owner_.registry.valid(entity))
            {
                std::vector<lux::simulation::ecs::Entity> references;
                for (const auto &component : object.components)
                {
                    if (component.schema->visit_references)
                    {
                        static_cast<void>(component.schema->visit_references(
                            owner_.registry, entity,
                            {&references, +[](void *state, auto referenced) noexcept {
                                 static_cast<std::vector<lux::simulation::ecs::Entity> *>(state)->push_back(referenced);
                             }}));
                    }
                }
                for (const auto reference : references)
                {
                    lux::partition::PartitionOrdinal partition;
                    if (owner_.loading.partitionOf(reference, partition))
                    {
                        add(partition);
                    }
                }
            }
        }
        for (const auto partition : partitions)
        {
            auto retained = owner_.loading.retain(partition);
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
        for (const auto &object : objects_)
        {
            bytes += object.row.label.capacity() + 1 + object.components.capacity() * sizeof(ObjectComponent);
            for (const auto &component : object.components)
            {
                bytes += component.bytes.capacity();
            }
        }
        return bytes;
    }
    editing::EditResult<editing::PreparedEditPtr> prepare(
        const editing::ApplyContext &context, editing::EditPreparationBudget &budget) const noexcept override
    {
        // Explicit accounted storage, not a promise about allocator buckets or nested provider heaps.
        const auto charge = [&](std::size_t count, std::size_t bytes) -> editing::EditResult<void> {
            if (bytes && count > budget.remaining() / bytes)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
            }
            return budget.reserve(count * bytes);
        };
        if (!charge(1, sizeof(Plan)) || !charge(owner_.rows.size(), sizeof(SceneObjectRow)) ||
            !charge(objects_.size(), sizeof(SceneObjectRow)) ||
            !charge(owner_.component_versions.size(), sizeof(ComponentNotice)))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
        }
        for (const auto &row : owner_.rows)
        {
            if (!charge(1, row.label.capacity() + 1))
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
            }
        }
        using IdentityPair = std::pair<lux::world::WorldObjectId, lux::simulation::ecs::Entity>;
        if (!charge(owner_.identities.size(), 2 * sizeof(IdentityPair)) ||
            !charge(objects_.size(), 2 * sizeof(IdentityPair) + sizeof(lux::simulation::ecs::Entity)))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
        }
        for (const auto &object : objects_)
        {
            if (!charge(1, object.row.label.capacity() + 1) ||
                !charge(object.components.size(), sizeof(ComponentNotice) +
                                                      sizeof(lux::simulation::ecs::DecodedComponent) +
                                                      sizeof(lux::simulation::ecs::Entity)))
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
            }
            for (const auto &component : object.components)
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
    SceneEditor &editor_;
    SceneObjects &owner_;
    editing::StateId base_;
    std::vector<ObjectContent> objects_;
    mutable std::vector<lux::scene::PartitionRetention> protections_;
    mutable std::vector<lux::partition::PartitionOrdinal> protected_partitions_;
    bool creation_;
    std::string label_;
    Id selection_before_;
};

} // namespace
editing::EditOperationPtr makeSceneObjectEdit(SceneEditor &editor, SceneObjects &objects, editing::StateId base,
                                              std::vector<ObjectContent> content, bool creation, std::string label)
{
    return std::make_unique<SceneObjectEdit>(editor, objects, base, std::move(content), creation, std::move(label));
}
} // namespace lux::editor::scene::detail
