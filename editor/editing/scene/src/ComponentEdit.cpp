#include <lux/engine/editor/editing/scene/SceneEditing.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <algorithm>

namespace lux::editor::scene
{
    namespace ecs = lux::simulation::ecs;
    namespace
    {
        constexpr std::size_t kComponentBytes = 16U * 1024U * 1024U;
        auto rejected(std::string_view message)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED, 0, message));
        }
    }

    editing::EditResult<void> SceneEditing::canAddComponent(SceneWriteTarget target, lux::cxx::TypeToken type)
        const noexcept
    {
        if (target.scene_id != scene_)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        auto current = writeTarget(target.entity);
        if (!current)
            return lux::cxx::unexpected(current.error());
        if (target.state.history != history_.id())
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::WRONG_HISTORY));
        if (target.state != current->state || target.revision != current->revision)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_BASE));
        const auto* schema = schemas_.find(type);
        const bool unsupported = !schema || !schema->create || !schema->decode_value ||
                                 schema->snapshot != ecs::EComponentSnapshotPolicy::COPY ||
                                 schema->semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                                 type == lux::cxx::typeToken<ecs::Parent>();
        if (unsupported)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::UNSUPPORTED_OPERATION));
        if (schema->operations.has(registry(), target.entity))
            return rejected("The object already has this component");
        if (!residency())
            return {};
        const auto& world = residency()->description();
        if (!world.partitionIndexes().empty())
            return rejected("Adding a component requires rebuilding this World's partition index");
        const auto schemas = world.schemas();
        const auto found = std::ranges::find(schemas, schema->id.name, &lux::world::WorldDataSchemaId::name);
        if (found == schemas.end())
            return rejected("The World does not declare this author schema");
        lux::partition::PartitionOrdinal partition;
        const auto identity = fieldIdentity(target);
        if (!identity.valid() || !residency()->partitionOf(target.entity, partition))
            return rejected("The object has no persistent partition");
        const auto* source = residency()->source(partition);
        if (!source)
            return rejected("The partition source is unavailable");
        const auto ordinal = static_cast<std::uint32_t>(found - schemas.begin());
        for (std::size_t i{}; i < source->objectCount(); ++i)
        {
            const auto object = source->objectAt(i);
            if (object.id() != identity)
                continue;
            for (std::size_t j{}; j < object.dataCount(); ++j)
                if (object.schemaOrdinalAt(j) == ordinal)
                    return rejected("The source retains undecoded data for this component");
        }
        return {};
    }

    class SceneEditing::AddComponent final : public editing::EditOperation
    {
        class Prepared final : public editing::PreparedEdit
        {
        public:
            Prepared(const AddComponent& operation, SceneWriteTarget target, std::optional<ecs::DecodedComponent> value)
                : operation_(operation), target_(target), value_(std::move(value))
            {}
            editing::EEditEffect effect() const noexcept override
            {
                return editing::EEditEffect::CHANGE;
            }

        private:
            void apply() noexcept override
            {
                auto& owner = operation_.owner_;
                if (value_)
                    std::move(*value_).installInto(owner.registry(), target_.entity);
                else
                    operation_.schema_->operations.erase(owner.registry(), target_.entity);
                lux::partition::PartitionOrdinal partition;
                if (owner.residency() && owner.residency()->partitionOf(target_.entity, partition))
                    static_cast<void>(owner.residency()->setDirty(partition, true));
            }
            void publish(const editing::CommitInfo& commit) noexcept override
            {
                auto& owner = operation_.owner_;
                const auto type = operation_.schema_->cpp_type;
                owner.recordComponentChange(target_.entity, type, commit.revision);
                const auto sequence = owner.componentVersion(target_.entity, type);
                if (!value_)
                    owner.forgetComponentChange(target_.entity, type);
                if (owner.changed)
                    owner.changed({target_.scene_id, target_.entity, type, commit.revision, false, sequence});
            }
            const AddComponent& operation_;
            SceneWriteTarget target_;
            std::optional<ecs::DecodedComponent> value_;
        };

    public:
        AddComponent(
            SceneEditing& owner,
            SceneWriteTarget target,
            const ecs::ComponentSchema& schema,
            std::vector<std::byte> bytes,
            std::vector<lux::scene::PartitionRetention> retention
        )
            : owner_(owner), target_(target), identity_(owner.fieldIdentity(target)), schema_(&schema),
              bytes_(std::move(bytes)), retention_(std::move(retention))
        {}
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
            return "Add component";
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return sizeof(*this) + bytes_.capacity() + retention_.capacity() * sizeof(lux::scene::PartitionRetention);
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            if (owner_.closed_ || !owner_.atSafePoint() || owner_.active())
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
            if (context.history != historyId() || owner_.history_.id() != historyId())
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::WRONG_HISTORY));
            auto target = owner_.replayTarget(target_, identity_);
            if (target.scene_id != owner_.scene_ || owner_.resolve(target.entity) == ecs::NullEntity)
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
            const bool adding = context.direction == editing::EDirection::FORWARD;
            if (schema_->operations.has(owner_.registry(), target.entity) == adding)
                return rejected("Component membership changed outside this operation");
            auto charged = budget.reserve(sizeof(Prepared) + schema_->operations.valueBytes() + bytes_.size());
            if (!charged)
                return lux::cxx::unexpected(charged.error());
            auto reserved = owner_.prepareComponentChanges(1);
            if (!reserved)
                return lux::cxx::unexpected(reserved.error());
            const ecs::WorldEntityMap empty;
            std::optional<ecs::DecodedComponent> value;
            if (adding)
            {
                if (owner_.residency() && !owner_.residency()->validateAdditional(0, schema_->operations.valueBytes()))
                    return rejected("The partition component capacity is exhausted");
                // A creation factory has no world/entity borrow. Defaults cannot introduce live entity references.
                auto decoded = schema_->decode_value(
                    schema_->version,
                    bytes_,
                    {nullptr,
                     +[](const void*, lux::world::WorldObjectId id
                      ) noexcept -> lux::cxx::expected<ecs::Entity, ecs::ComponentDecodeFailure> {
                         if (!id.valid())
                             return ecs::NullEntity;
                         return lux::cxx::unexpected(
                             ecs::ComponentDecodeFailure{ecs::EComponentDecodeError::UNRESOLVED_REFERENCE, 0, id}
                         );
                     }},
                    schema_->code_lifetime
                );
                if (!decoded)
                    return rejected("The component default could not be decoded");
                value.emplace(std::move(*decoded));
            }
            else
            {
                auto capture = schema_->capture(owner_.registry(), target.entity, schema_->code_lifetime);
                if (!capture)
                    return rejected("The component cannot be captured for removal");
                auto encoded = capture->encode(empty, kComponentBytes);
                if (!encoded || *encoded != bytes_)
                    return rejected("The component changed outside this history");
            }
            return editing::PreparedEditPtr{std::make_unique<Prepared>(*this, target, std::move(value))};
        }

    private:
        SceneEditing& owner_;
        SceneWriteTarget target_;
        lux::world::WorldObjectId identity_;
        const ecs::ComponentSchema* schema_;
        std::vector<std::byte> bytes_;
        std::vector<lux::scene::PartitionRetention> retention_;
    };

    editing::EditResult<editing::ApplyResult> SceneEditing::addComponent(
        SceneWriteTarget target,
        lux::cxx::TypeToken type
    )
    {
        const auto admitted = canAddComponent(target, type);
        if (!admitted)
            return lux::cxx::unexpected(admitted.error());
        const auto& schema = *schemas_.find(type);
        auto captured = schema.create(schema.code_lifetime);
        if (!captured)
            return rejected("The component creation factory rejected the request");
        const ecs::WorldEntityMap empty;
        auto encoded = captured->encode(empty, kComponentBytes);
        if (!encoded)
            return rejected("The component default could not be captured without world references");
        editing::EditOperationPtr operation = std::make_unique<AddComponent>(
            *this,
            target,
            schema,
            std::move(*encoded),
            retainFieldTargets(target, type)
        );
        return executeField(operation);
    }
}
