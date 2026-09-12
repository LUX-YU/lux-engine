#include <lux/engine/editor/sessions/scene/detail/SceneEditState.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
namespace lux::editor::sessions::detail
{
    namespace
    {
        auto fail(ESceneError code, SessionId session) noexcept
        {
            return lux::cxx::unexpected(SceneFailure{code, session});
        }
        auto editFail(editing::EEditError code) noexcept
        {
            return lux::cxx::unexpected(editing::makeEditFailure(code));
        }
        bool equal(const lux::simulation::ecs::Transform3D &a,
                   const lux::simulation::ecs::Transform3D &b) noexcept
        {
            return (a.translation.array() == b.translation.array()).all() &&
                   (a.scale.array() == b.scale.array()).all() &&
                   (a.rotation.coeffs().array() == b.rotation.coeffs().array()).all();
        }
        bool equal(const SceneAuthorObject &a, const SceneAuthorObject &b) noexcept
        {
            return a.object == b.object && a.entity == b.entity &&
                   bool(a.transform) == bool(b.transform) && bool(a.light) == bool(b.light) &&
                   (!a.transform || equal(*a.transform, *b.transform)) &&
                   (!a.light || a.light->value == b.light->value);
        }
        bool valid(const lux::simulation::ecs::Transform3D &value) noexcept
        {
            return value.translation.allFinite() && value.scale.allFinite() &&
                   value.rotation.coeffs().allFinite() &&
                   std::abs(value.rotation.squaredNorm() - 1.0) < 1e-8;
        }
        bool valid(const lux::simulation::ecs::Light3D &value) noexcept
        {
            const auto &v = value.value;
            const auto nonnegative = [](auto number) { return std::isfinite(number) && number >= 0; };
            const bool colors = std::all_of(v.color.begin(), v.color.end(), nonnegative);
            const bool scalars = nonnegative(v.intensity) && nonnegative(v.range) &&
                nonnegative(v.attenuation_constant) && nonnegative(v.attenuation_linear) &&
                nonnegative(v.attenuation_quadratic) && nonnegative(v.inner_cone_angle) &&
                nonnegative(v.outer_cone_angle) && nonnegative(v.area_size[0]) && nonnegative(v.area_size[1]) &&
                nonnegative(v.shadow_bias) && nonnegative(v.shadow_normal_bias);
            const bool shape = v.type <= lux::rdesc::ELightType::AREA &&
                v.inner_cone_angle <= v.outer_cone_angle && v.outer_cone_angle < 1.5707964F &&
                v.shadow_map_size && v.shadow_map_size <= 16384 && v.cascade_count &&
                v.cascade_count <= lux::rdesc::kLightCascadeSlots;
            const bool splits = std::all_of(v.cascade_splits.begin(), v.cascade_splits.end(),
                [&](float number) { return nonnegative(number) && number <= 1; });
            return colors && scalars && shape && splits;
        }
    }
    class SceneEditState::Plan final : public editing::PreparedEdit
    {
    public:
        Plan(SceneEditState &state, std::size_t index, std::unique_ptr<SceneAuthorObject> next, bool changed) noexcept
            : state_(state), index_(index), next_(std::move(next)), changed_(changed) {}
        editing::EEditEffect effect() const noexcept override
        {
            return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
        }
    private:
        void apply() noexcept override
        {
            // Exactly one prepared owner swap. No Registry, observer, allocation or renderer call.
            state_.objects_[index_].swap(next_);
            state_.projection_dirty_ = true;
        }
        void publish(const editing::CommitInfo &info) noexcept override { state_.publish(info); }
        SceneEditState &state_;
        std::size_t index_;
        std::unique_ptr<SceneAuthorObject> next_;
        bool changed_;
    };
    class SceneEditState::Operation final : public editing::EditOperation
    {
    public:
        Operation(SceneEditState &state, std::size_t index, SceneAuthorObject before, SceneAuthorObject after,
                  ESceneProperty property, editing::StateId base) noexcept
            : state_(state), index_(index), before_(std::move(before)), after_(std::move(after)),
              property_(property), base_(base) {}
        editing::HistoryId historyId() const noexcept override { return state_.history_->id(); }
        editing::StateId baseState() const noexcept override { return base_; }
        std::string_view label() const noexcept override
        {
            return property_ == ESceneProperty::TRANSFORM ? "Edit transform" : "Edit light";
        }
        std::size_t retainedBytesUpperBound() const noexcept override { return sizeof(*this); }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext &context, editing::EditPreparationBudget &budget) const noexcept override
        {
            const bool forward = context.direction == editing::EDirection::FORWARD;
            const auto &expected = forward ? before_ : after_;
            const auto &next = forward ? after_ : before_;
            if (index_ >= state_.objects_.size() || !equal(*state_.objects_[index_], expected))
                return editFail(editing::EEditError::PRECONDITION_FAILED);
            const auto reserved = budget.reserve(sizeof(Plan) + sizeof(SceneAuthorObject));
            if (!reserved)
                return lux::cxx::unexpected(reserved.error());
            try
            {
                auto prepared = std::make_unique<SceneAuthorObject>(next);
                editing::PreparedEditPtr plan =
                    std::make_unique<Plan>(state_, index_, std::move(prepared), !equal(expected, next));
                return plan;
            }
            catch (const std::bad_alloc &)
            {
                return editFail(editing::EEditError::ALLOCATION_FAILURE);
            }
        }
    private:
        SceneEditState &state_;
        std::size_t index_;
        SceneAuthorObject before_, after_;
        ESceneProperty property_;
        editing::StateId base_;
    };
    SceneResult<std::unique_ptr<SceneEditState>> SceneEditState::prepare(const SceneEditInput &input) noexcept
    {
        const auto session = input.source.id;
        if (!input.source.scene || !input.source.metadata || input.objects.empty())
            return fail(ESceneError::INVALID_ARGUMENT, session);
        const auto &registry = input.source.scene->registry();
        try
        {
            auto state = std::make_unique<SceneEditState>();
            state->session_ = session;
            state->objects_.reserve(input.objects.size());
            for (const auto &object : input.objects)
            {
                if (!object.object.valid() || !registry.valid(object.entity) || (!object.transform && !object.light))
                    return fail(ESceneError::INVALID_ARGUMENT, session);
                const auto duplicate = std::any_of(state->objects_.begin(), state->objects_.end(),
                    [&](const auto &old) {
                    return old->object == object.object || old->entity == object.entity;
                });
                if (duplicate)
                    return fail(ESceneError::INVALID_ARGUMENT, session);
                if (object.transform)
                {
                    const auto *schema = input.source.metadata->getComponentMeta(
                        lux::cxx::typeToken<lux::simulation::ecs::Transform3D>());
                    if (!schema || schema->id.name != "lux.ecs.Transform3D" || schema->version != 1)
                        return fail(ESceneError::UNSUPPORTED_EDIT, session);
                    if (!valid(*object.transform))
                        return fail(ESceneError::INVALID_ARGUMENT, session);
                    const auto *actual = registry.try_get<lux::simulation::ecs::Transform3D>(object.entity);
                    if (!actual || !equal(*actual, *object.transform))
                        return fail(ESceneError::STALE_CONTENT, session);
                }
                if (object.light)
                {
                    const auto *schema = input.source.metadata->getComponentMeta(
                        lux::cxx::typeToken<lux::simulation::ecs::Light3D>());
                    if (!schema || schema->id.name != "lux.ecs.Light3D" || schema->version != 2)
                        return fail(ESceneError::UNSUPPORTED_EDIT, session);
                    if (!valid(*object.light))
                        return fail(ESceneError::INVALID_ARGUMENT, session);
                    const auto *actual = registry.try_get<lux::simulation::ecs::Light3D>(object.entity);
                    if (!actual || actual->value != object.light->value)
                        return fail(ESceneError::STALE_CONTENT, session);
                }
                state->objects_.push_back(std::make_unique<SceneAuthorObject>(object));
            }
            return state;
        }
        catch (const std::bad_alloc &)
        {
            return fail(ESceneError::ALLOCATION_FAILURE, session);
        }
    }
    void SceneEditState::bind(editing::EditHistory &history, SceneSession &owner,
                             void (*published)(SceneSession &, const editing::CommitInfo &) noexcept) noexcept
    {
        history_ = &history;
        owner_ = &owner;
        published_ = published;
    }
    SceneResult<std::size_t> SceneEditState::index(SceneObjectRef target) const noexcept
    {
        if (target.session != session_)
            return fail(ESceneError::STALE_SESSION, session_);
        for (std::size_t i = 0; i < objects_.size(); ++i)
            if (objects_[i]->object == target.object)
                return i;
        return fail(ESceneError::STALE_CONTENT, session_);
    }
    bool SceneEditState::matches(PropertyGesture token) const noexcept
    {
        return gesture_ && token.session == session_ && token.sequence == gesture_->token.sequence;
    }
    bool SceneEditState::gesturing() const noexcept { return gesture_.has_value(); }
    SceneResult<PropertyGesture> SceneEditState::begin(SceneObjectRef target, ESceneProperty property) noexcept
    {
        if (gesture_)
            return fail(ESceneError::BUSY, session_);
        const auto found = index(target);
        if (!found)
            return lux::cxx::unexpected(found.error());
        const auto &object = *objects_[*found];
        const bool supported = property == ESceneProperty::TRANSFORM ? bool(object.transform) : bool(object.light);
        if (!supported)
            return fail(ESceneError::UNSUPPORTED_EDIT, session_);
        if (next_gesture_ == (std::numeric_limits<std::uint64_t>::max)() ||
            preview_revision_ == (std::numeric_limits<std::uint64_t>::max)())
            return fail(ESceneError::CONTRACT_FAILURE, session_);
        gesture_.emplace(Gesture{{session_, ++next_gesture_}, *found, property, object, {}});
        return gesture_->token;
    }
    SceneResult<void> SceneEditState::preview(PropertyGesture token,
                                              const lux::simulation::ecs::Transform3D &value) noexcept
    {
        if (!matches(token) || gesture_->property != ESceneProperty::TRANSFORM)
            return fail(ESceneError::STALE_CONTENT, session_);
        if (!valid(value))
            return fail(ESceneError::INVALID_ARGUMENT, session_);
        if (preview_revision_ >= (std::numeric_limits<std::uint64_t>::max)() - 1)
            return fail(ESceneError::CONTRACT_FAILURE, session_);
        ++preview_revision_;
        gesture_->preview.transform = value;
        gesture_->pending.reset();
        projection_dirty_ = true;
        return {};
    }
    SceneResult<void> SceneEditState::preview(PropertyGesture token,
                                              const lux::simulation::ecs::Light3D &value) noexcept
    {
        if (!matches(token) || gesture_->property != ESceneProperty::LIGHT)
            return fail(ESceneError::STALE_CONTENT, session_);
        if (!valid(value))
            return fail(ESceneError::INVALID_ARGUMENT, session_);
        if (preview_revision_ >= (std::numeric_limits<std::uint64_t>::max)() - 1)
            return fail(ESceneError::CONTRACT_FAILURE, session_);
        ++preview_revision_;
        gesture_->preview.light = value;
        gesture_->pending.reset();
        projection_dirty_ = true;
        return {};
    }
    SceneResult<editing::ApplyResult> SceneEditState::commit(PropertyGesture token, ESceneProperty property) noexcept
    {
        if (!matches(token) || gesture_->property != property)
            return fail(ESceneError::STALE_CONTENT, session_);
        try
        {
            if (!gesture_->pending)
            {
                const auto view = history_->view();
                if (!view)
                    return fail(ESceneError::BUSY, session_);
                gesture_->pending = std::make_unique<Operation>(*this, gesture_->index,
                    *objects_[gesture_->index], gesture_->preview, gesture_->property, view->snapshot.current);
            }
        }
        catch (const std::bad_alloc &)
        {
            return fail(ESceneError::ALLOCATION_FAILURE, session_);
        }
        const auto result = history_->execute(gesture_->pending);
        if (!result)
        {
            SceneFailure failure{ESceneError::HISTORY_FAILURE, session_};
            failure.history = result.error();
            return lux::cxx::unexpected(failure);
        }
        gesture_.reset();
        projection_dirty_ = true;
        return *result;
    }
    SceneResult<void> SceneEditState::cancel(PropertyGesture token, ESceneProperty property) noexcept
    {
        if (!matches(token) || gesture_->property != property)
            return fail(ESceneError::STALE_CONTENT, session_);
        cancel();
        return {};
    }
    void SceneEditState::cancel() noexcept
    {
        if (gesture_)
        {
            ++preview_revision_; // begin/preview always reserve this cancellation increment.
            gesture_.reset();
            projection_dirty_ = true;
        }
    }
    SceneResult<SceneAuthorObject> SceneEditState::read(SceneObjectRef target, bool preview) const noexcept
    {
        const auto found = index(target);
        if (!found)
            return lux::cxx::unexpected(found.error());
        return preview && gesture_ && gesture_->index == *found ? gesture_->preview : *objects_[*found];
    }
    std::optional<SceneObjectRef> SceneEditState::authored(lux::simulation::ecs::Entity entity) const noexcept
    {
        for (const auto &object : objects_)
            if (object->entity == entity)
                return SceneObjectRef{session_, object->object};
        return std::nullopt;
    }
    SceneResult<void> SceneEditState::project(lux::simulation::ecs::Registry &registry) noexcept
    {
        if (!projection_dirty_)
            return {};
        try
        {
            for (std::size_t i = 0; i < objects_.size(); ++i)
            {
                const auto &value = gesture_ && gesture_->index == i ? gesture_->preview : *objects_[i];
                const bool valid_entity = registry.valid(value.entity);
                const bool has_transform = !value.transform ||
                    (valid_entity && registry.all_of<lux::simulation::ecs::Transform3D>(value.entity));
                const bool has_light = !value.light ||
                    (valid_entity && registry.all_of<lux::simulation::ecs::Light3D>(value.entity));
                if (!valid_entity || !has_transform || !has_light)
                {
                    projection_failure_ = SceneFailure{ESceneError::STALE_ENTITY, session_};
                    projection_failure_->context.subject = entt::to_integral(value.entity);
                    constexpr char message[] = "Author projection target/component is missing";
                    std::copy_n(message, sizeof(message), projection_failure_->context.message.begin());
                    return lux::cxx::unexpected(*projection_failure_);
                }
            }
            // Projection is a separate fallible preparation boundary, never PreparedEdit::apply.
            for (std::size_t i = 0; i < objects_.size(); ++i)
            {
                const auto &value = gesture_ && gesture_->index == i ? gesture_->preview : *objects_[i];
                if (value.transform && (projection_failure_ ||
                    !equal(registry.get<lux::simulation::ecs::Transform3D>(value.entity), *value.transform)))
                    registry.patch<lux::simulation::ecs::Transform3D>(value.entity,
                        [&](auto &component) { component = *value.transform; });
                if (value.light && (projection_failure_ ||
                    registry.get<lux::simulation::ecs::Light3D>(value.entity).value != value.light->value))
                    registry.patch<lux::simulation::ecs::Light3D>(value.entity,
                        [&](auto &component) { component = *value.light; });
            }
        }
        catch (const std::bad_alloc &)
        {
            projection_failure_ = SceneFailure{ESceneError::ALLOCATION_FAILURE, session_};
            return lux::cxx::unexpected(*projection_failure_);
        }
        projection_failure_.reset();
        projection_dirty_ = false;
        return {};
    }
    void SceneEditState::publish(const editing::CommitInfo &info) noexcept
    {
        published_(*owner_, info);
    }
}
