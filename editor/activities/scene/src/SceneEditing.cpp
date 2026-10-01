#include <lux/engine/editor/editing/scene/SceneEditing.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/description/Visual.hpp>
#include <algorithm>
#include <array>
#include <exception>
namespace lux::editor::scene
{
    namespace
    {
        bool componentLess(const ComponentNotice& a, const ComponentNotice& b) noexcept
        {
            return a.entity != b.entity ? a.entity < b.entity : a.component.hash() < b.component.hash();
        }
        constexpr editing::HistoryLimits kHistoryLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};
        struct EditingGuard final
        {
            explicit EditingGuard(bool& flag) noexcept : flag(flag)
            {
                flag = true;
            }
            ~EditingGuard()
            {
                flag = false;
            }
            bool& flag;
        };
    }
    detail::RegistryFieldEdit::~RegistryFieldEdit() = default;
    SceneEditing::SceneEditing(
        lux::scene::SceneRuntime& runtime,
        lux::scene::SceneInstanceId scene,
        const lux::simulation::ecs::ComponentSchemaSet& schemas,
        editing::EditHistory& history
    ) noexcept
        : runtime_(runtime), scene_(scene), schemas_(schemas), history_(history)
    {}
    lux::simulation::ecs::Entity SceneEditing::resolve(lux::simulation::ecs::Entity ref) const noexcept
    {
        const auto borrowed = std::as_const(runtime_).borrowInstance(scene_);
        return borrowed && borrowed->get().valid(ref) ? ref : lux::simulation::ecs::NullEntity;
    }
    lux::simulation::ecs::Registry& SceneEditing::registry() const noexcept
    {
        const auto borrowed = runtime_.borrowInstance(scene_);
        if (!borrowed)
            std::terminate(); // Private write access follows admission; no Runtime boundary occurs in between.
        return borrowed->get();
    }
    lux::scene::WorldResidency* SceneEditing::residency() const noexcept
    {
        const auto borrowed = runtime_.borrowInstance(scene_);
        return borrowed ? borrowed->get().ctx().find<lux::scene::WorldResidency>() : nullptr;
    }
    bool SceneEditing::atSafePoint() const noexcept
    {
        return bool(runtime_.borrowInstance(scene_));
    }
    editing::EditResult<void> SceneEditing::prepareComponentChanges(std::size_t count) noexcept
    {
        if (count > UINT64_MAX - next_component_change_)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
        component_changes_.reserve(component_changes_.size() + count);
        return {};
    }
    void SceneEditing::recordComponentChange(
        lux::simulation::ecs::Entity object,
        lux::cxx::TypeToken type,
        editing::Revision revision
    ) noexcept
    {
        ComponentNotice notice{scene_, object, type, revision, false, next_component_change_++};
        auto entry = std::ranges::lower_bound(component_changes_, notice, componentLess);
        if (entry != component_changes_.end() && entry->entity == object && entry->component == type)
            *entry = notice;
        else
        {
            if (component_changes_.size() == component_changes_.capacity())
                std::terminate();
            component_changes_.insert(entry, notice);
        }
    }
    void SceneEditing::forgetComponentChanges(lux::simulation::ecs::Entity object) noexcept
    {
        std::erase_if(component_changes_, [&](const auto& value) { return value.entity == object; });
    }
    void SceneEditing::forgetComponentChange(lux::simulation::ecs::Entity object, lux::cxx::TypeToken type) noexcept
    {
        std::erase_if(component_changes_, [&](const auto& value) {
            return value.entity == object && value.component == type;
        });
    }
    editing::EditResult<void> SceneEditing::checkAdmission() const noexcept
    {
        if (closed_)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
        if (!std::as_const(runtime_).borrowInstance(scene_))
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        if (busy_ || !atSafePoint())
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        return admission ? admission() : editing::EditResult<void>{};
    }
    std::string_view SceneEditing::writeRestriction() const noexcept
    {
        return checkAdmission() ? std::string_view{} : std::string_view{"Scene editing is unavailable"};
    }
    const void* SceneEditing::component(lux::simulation::ecs::Entity object, lux::cxx::TypeToken type) const noexcept
    {
        if (closed_)
            return nullptr;
        const auto borrowed = std::as_const(runtime_).borrowInstance(scene_);
        if (!borrowed || !borrowed->get().valid(object))
            return nullptr;
        const auto* schema = schemas_.find(type);
        return schema ? schema->operations.get(borrowed->get(), object) : nullptr;
    }
    editing::EditResult<SceneWriteTarget> SceneEditing::writeTarget(lux::simulation::ecs::Entity object) const noexcept
    {
        auto admitted = checkAdmission();
        if (!admitted)
            return lux::cxx::unexpected(admitted.error());
        if (active())
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        if (resolve(object) == lux::simulation::ecs::NullEntity)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        const auto state = history_.view();
        if (!state)
            return lux::cxx::unexpected(state.error());
        return SceneWriteTarget{scene_, object, state->snapshot.current, state->snapshot.revision};
    }
    std::uint64_t SceneEditing::componentVersion(lux::simulation::ecs::Entity object, lux::cxx::TypeToken type)
        const noexcept
    {
        const ComponentNotice key{scene_, object, type};
        const auto entry = std::ranges::lower_bound(component_changes_, key, componentLess);
        return entry != component_changes_.end() && entry->entity == object && entry->component == type
                   ? entry->sequence
                   : 0;
    }

    lux::world::WorldObjectId SceneEditing::fieldIdentity(const SceneWriteTarget& target) const noexcept
    {
        const bool has_residency = residency() != nullptr;
        const bool is_current_instance = target.scene_id == scene_;
        const bool can_resolve_identity = has_residency && is_current_instance;
        return can_resolve_identity ? residency()->identities().object(resolve(target.entity))
                                    : lux::world::WorldObjectId{};
    }

    SceneWriteTarget SceneEditing::replayTarget(SceneWriteTarget target, lux::world::WorldObjectId identity)
        const noexcept
    {
        if (residency() && identity.valid() && target.scene_id == scene_)
        {
            target.entity = residency()->identities().entity(identity);
        }
        return target;
    }

    editing::EditResult<void*> SceneEditing::fieldAccess(
        const SceneWriteTarget& target,
        lux::cxx::TypeToken type,
        bool require_current
    )
    {
        if (closed_)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
        }
        if (target.scene_id != scene_)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        }
        if (target.state.history != history_.id())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::WRONG_HISTORY));
        }
        if (require_current)
        {
            auto current = writeTarget(target.entity);
            if (!current)
            {
                return lux::cxx::unexpected(current.error());
            }
            if (target.state != current->state || target.revision != current->revision)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_BASE));
            }
        }
        if (!std::as_const(runtime_).borrowInstance(scene_))
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        if (!atSafePoint())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        const auto* schema = schemas_.find(type);
        if (!schema || schema->semantic_kind == lux::simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
            schema->snapshot != lux::simulation::ecs::EComponentSnapshotPolicy::COPY ||
            type == lux::cxx::typeToken<lux::simulation::ecs::Parent>())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::UNSUPPORTED_OPERATION));
        }
        const auto* value = component(target.entity, type);
        if (!value)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        if (require_current || residency())
        {
            if (auto prepared = prepareComponentChanges(1); !prepared)
                return lux::cxx::unexpected(prepared.error());
            auto& versions = component_changes_;
            const ComponentNotice key{target.scene_id, target.entity, type};
            auto entry = std::ranges::lower_bound(versions, key, componentLess);
            if (entry == versions.end() || entry->entity != target.entity || entry->component != type)
            {
                versions.insert(entry, key);
            }
        }
        // The private write boundary is the only place that turns a schema's read
        // borrow into a prepared edit.
        return const_cast<void*>(value);
    }

    editing::EditResult<void> SceneEditing::checkFieldSize(std::size_t bytes) const noexcept
    {
        if (bytes > kHistoryLimits.max_staging_bytes / 2)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
        }
        return {};
    }

    editing::EditResult<void> SceneEditing::validateFieldValue(
        lux::cxx::TypeToken type,
        const void* before,
        const void* next
    ) const
    {
        const auto invalid = [](const char* message) {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED, 0, message));
        };
        using Projection = decltype(lux::scene::Camera::projection);
        if (type == lux::cxx::typeToken<Projection>())
        {
            lux::scene::Camera camera;
            camera.projection = *static_cast<const Projection*>(next);
            if (!lux::scene::cameraProjection(camera, 1.0))
            {
                return invalid("The camera projection requires a positive extent/FOV and 0 < near < far");
            }
        }
        if (type == lux::cxx::typeToken<lux::rdesc::MeshVisualDescription>())
        {
            const auto& original = *static_cast<const lux::rdesc::MeshVisualDescription*>(before);
            const auto& value = *static_cast<const lux::rdesc::MeshVisualDescription*>(next);
            const auto valid = [&](asset::AssetId old, asset::AssetId id, std::uint32_t magic) {
                if (id.isNull() || id == old)
                {
                    return true;
                }
                return acceptsAsset && acceptsAsset(id, magic);
            };
            if (!valid(original.mesh, value.mesh, asset::MeshAsset::primary_magic) ||
                !valid(original.material, value.material, asset::MaterialAsset::primary_magic))
            {
                return invalid("The replacement asset is unavailable or has the wrong type");
            }
        }
        if (type == lux::cxx::typeToken<lux::rdesc::LightDescription>())
        {
            const auto& value = *static_cast<const lux::rdesc::LightDescription*>(next);
            const auto finite = [](float number) { return std::isfinite(number); };
            const std::array scalars{
                value.intensity,
                value.range,
                value.attenuation_constant,
                value.attenuation_linear,
                value.attenuation_quadratic,
                value.inner_cone_angle,
                value.outer_cone_angle,
                value.shadow_bias,
                value.shadow_normal_bias
            };
            const bool valid_numbers =
                std::ranges::all_of(scalars, finite) && std::ranges::all_of(value.color, finite) &&
                std::ranges::all_of(value.area_size, finite) && std::ranges::all_of(value.cascade_splits, finite);
            const bool valid_shape =
                value.type <= lux::rdesc::ELightType::AREA && value.cascade_count <= lux::rdesc::kLightCascadeSlots;
            if (!valid_numbers || !valid_shape)
            {
                return invalid("The light contains a non-finite value or an invalid "
                               "type/cascade count");
            }
        }
        return {};
    }

    std::vector<lux::scene::PartitionRetention> SceneEditing::retainFieldTargets(
        const SceneWriteTarget& target,
        lux::cxx::TypeToken type
    ) const
    {
        if (!residency())
            return {};
        std::vector<lux::simulation::ecs::Entity> entities{resolve(target.entity)};
        const auto* schema = schemas_.find(type);
        if (schema && schema->visit_references && schema->operations.has(registry(), entities.front()))
            static_cast<void>(schema->visit_references(
                registry(),
                entities.front(),
                {&entities,
                 +[](void* state, auto entity) noexcept {
                     static_cast<std::vector<lux::simulation::ecs::Entity>*>(state)->push_back(entity);
                 }}
            ));
        std::vector<lux::partition::PartitionOrdinal> partitions;
        std::vector<lux::scene::PartitionRetention> retained;
        for (auto entity : entities)
        {
            lux::partition::PartitionOrdinal partition;
            if (residency()->partitionOf(entity, partition) &&
                std::ranges::find(partitions, partition) == partitions.end())
            {
                auto token = residency()->retain(partition);
                if (!token)
                {
                    std::terminate();
                }
                retained.push_back(std::move(*token));
                partitions.push_back(partition);
            }
        }
        return retained;
    }
    std::size_t SceneEditing::fieldResidencyCount() const noexcept
    {
        return residency() ? residency()->statistics().resident_partitions : 0;
    }

    void SceneEditing::fieldChanged(const SceneWriteTarget& target, lux::cxx::TypeToken type, bool field_edit) noexcept
    {
        const auto history = history_.view();
        ComponentNotice notice{target.scene_id, target.entity, type, history->snapshot.revision, field_edit};
        notice.sequence = next_component_change_++;
        auto entry = std::ranges::lower_bound(component_changes_, notice, componentLess);
        if (entry != component_changes_.end() && entry->entity == target.entity && entry->component == type)
        {
            entry->sequence = notice.sequence;
            if (!field_edit)
            {
                entry->revision = notice.revision;
            }
        }
        const auto entity = resolve(target.entity);
        lux::partition::PartitionOrdinal partition;
        if (residency() && residency()->partitionOf(entity, partition))
            static_cast<void>(residency()->setDirty(partition, true));
        schemas_.find(type)->operations.notifyUpdated(registry(), entity);
        if (changed)
            changed(notice);
    }

    editing::EditResult<editing::ApplyResult> SceneEditing::executeField(editing::EditOperationPtr& operation)
    {
        const auto admitted = checkAdmission();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }

        if (busy_ || field_edit_.index() != 0)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        EditingGuard guard(busy_);
        return history_.execute(operation);
    }

    editing::EditResult<FieldEditToken> SceneEditing::adoptFieldEdit(
        std::string origin,
        std::unique_ptr<detail::RegistryFieldEdit>& operation
    )
    {
        const auto admitted = checkAdmission();
        if (!admitted)
        {
            return lux::cxx::unexpected(admitted.error());
        }

        if (busy_ || field_edit_.index() != 0)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        if (origin.empty() || !operation)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        if (next_field_edit_ == (std::numeric_limits<std::uint64_t>::max)())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
        }
        FieldEditToken token{history_.id(), next_field_edit_++, std::move(origin)};
        field_edit_.emplace<FieldGesture>(token, std::move(operation));
        return token;
    }

    bool SceneEditing::fieldEditWritable(const FieldEditToken& token) const noexcept
    {
        const auto* edit = std::get_if<FieldGesture>(&field_edit_);
        return edit && edit->token == token && !busy_ && !closed_ && writeRestriction().empty() &&
               static_cast<const detail::RegistryFieldEdit&>(*edit->operation).writable();
    }

    editing::EditResult<void> SceneEditing::fieldEdited(const FieldEditToken& token)
    {
        if (busy_)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        auto* edit = std::get_if<FieldGesture>(&field_edit_);
        if (!edit || edit->token != token)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        }
        EditingGuard guard(busy_);
        return static_cast<detail::RegistryFieldEdit&>(*edit->operation).changed();
    }

    editing::EditResult<void> SceneEditing::finishFieldEdits()
    {
        auto* edit = std::get_if<FieldGesture>(&field_edit_);
        if (!edit)
        {
            return {};
        }
        auto finished = finishFieldEdit(edit->token);
        if (!finished)
        {
            return lux::cxx::unexpected(finished.error());
        }
        return {};
    }

    editing::EditResult<editing::ApplyResult> SceneEditing::finishFieldEdit(const FieldEditToken& token)
    {
        if (busy_)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
        }
        auto* field_edit = std::get_if<FieldGesture>(&field_edit_);
        if (!field_edit || field_edit->token != token)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
        }
        EditingGuard guard(busy_);
        auto captured = static_cast<detail::RegistryFieldEdit&>(*field_edit->operation).captureAfter();
        if (!captured)
        {
            return lux::cxx::unexpected(captured.error());
        }
        auto result = history_.execute(field_edit->operation);
        if (result)
        {
            field_edit_.emplace<std::monostate>();
        }
        return result;
    }

}
