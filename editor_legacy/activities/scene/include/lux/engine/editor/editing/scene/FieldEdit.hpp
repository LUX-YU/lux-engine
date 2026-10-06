#pragma once

#include <lux/engine/editor/editing/scene/SceneEditing.hpp>
#include <lux/engine/editor/scene/FieldValue.hpp>

namespace lux::editor::scene
{
    namespace detail
    {
        template <class Component, class Value, class Access> class TFieldEdit final : public RegistryFieldEdit
        {
        public:
            TFieldEdit(
                SceneEditing& owner,
                SceneWriteTarget target,
                std::string_view field,
                std::string_view label,
                Access access,
                const Value& before,
                const Value& after,
                bool already_applied
            )
                : owner_(owner), target_(target), identity_(owner.fieldIdentity(target)), field_(field), label_(label),
                  access_(std::move(access)), before_(before), already_applied_(already_applied)
            {
                protections_ = owner_.retainFieldTargets(target_, lux::cxx::typeToken<Component>());
                // Account the maximum additional distinct after-value partitions
                // before EditHistory reads retainedBytesUpperBound().
                protections_.reserve(protections_.size() + owner_.fieldResidencyCount());
                if (!already_applied_)
                {
                    after_ = after;
                    captured_ = true;
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
                return label_;
            }

            std::size_t retainedBytesUpperBound() const noexcept override
            {
                return addFieldBytes(
                    sizeof(*this) + field_.capacity() + label_.capacity() + 2 +
                        protections_.capacity() * sizeof(lux::scene::PartitionRetention),
                    addFieldBytes(TFieldValue<Value>::bytes(before_), TFieldValue<Value>::bytes(after_))
                );
            }

            editing::EditResult<editing::PreparedEditPtr> prepare(
                const editing::ApplyContext& context,
                editing::EditPreparationBudget& budget
            ) const noexcept override
            {
                auto live = locate(false);
                if (!live)
                {
                    return lux::cxx::unexpected(live.error());
                }
                const bool backwards = context.direction == editing::EDirection::BACKWARD;
                const bool adopt_existing = already_applied_ && context.kind == editing::EApplyKind::EXECUTE;
                const auto& expected = backwards || adopt_existing ? after_ : before_;
                const auto& next = backwards ? before_ : after_;
                if (!TFieldValue<Value>::equal(**live, expected))
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
                }
                if (auto valid = validate(next, context.kind == editing::EApplyKind::EXECUTE); !valid)
                {
                    return lux::cxx::unexpected(valid.error());
                }
                const auto staging =
                    adopt_existing ? sizeof(Prepared) : sizeof(Prepared) + TFieldValue<Value>::bytes(next);
                if (auto reserved = budget.reserve(staging); !reserved)
                {
                    return lux::cxx::unexpected(reserved.error());
                }
                if (adopt_existing)
                {
                    return editing::PreparedEditPtr{new Prepared(*this)};
                }
                return editing::PreparedEditPtr{new Prepared(*this, **live, next)};
            }

            bool writable() const noexcept override
            {
                return !captured_;
            }

            editing::EditResult<void> changed() noexcept override
            {
                if (captured_)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::BUSY));
                }
                auto live = locate(false);
                if (!live)
                {
                    return lux::cxx::unexpected(live.error());
                }
                auto valid = validate(**live, true);
                if (valid)
                {
                    valid = owner_.checkFieldSize(TFieldValue<Value>::bytes(**live));
                }
                if (!valid)
                {
                    // Invalid input is not published. Restore the gesture's
                    // initial value on this failure path; normal updates copy
                    // neither the field nor its before value.
                    Value restored(before_);
                    TFieldValue<Value>::swap(**live, restored);
                    owner_.fieldChanged(target_, lux::cxx::typeToken<Component>(), true);
                    return valid;
                }
                owner_.fieldChanged(target_, lux::cxx::typeToken<Component>(), true);
                return {};
            }

            editing::EditResult<void> captureAfter() override
            {
                if (captured_)
                {
                    return {};
                }
                auto live = locate(false);
                if (!live)
                {
                    return lux::cxx::unexpected(live.error());
                }
                auto valid = validate(**live, true);
                if (!valid)
                {
                    return valid;
                }
                auto size = owner_.checkFieldSize(TFieldValue<Value>::bytes(**live));
                if (!size)
                {
                    return size;
                }
                after_ = **live;
                protections_.reserve(protections_.size() + owner_.fieldResidencyCount());
                captured_ = true;
                return {};
            }

        private:
            editing::EditResult<Value*> locate(bool current) const
            {
                auto component = owner_.fieldAccess(
                    owner_.replayTarget(target_, identity_),
                    lux::cxx::typeToken<Component>(),
                    current
                );
                if (!component)
                {
                    return lux::cxx::unexpected(component.error());
                }
                auto* value = access_(*static_cast<Component*>(*component));
                if (!value)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
                }
                return value;
            }

            editing::EditResult<void> validate(const Value& value, bool admission) const
            {
                if (!TFieldValue<Value>::valid(value))
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
                }
                if constexpr (requires { access_.validate(owner_, value); })
                {
                    const auto valid = access_.validate(owner_, value);
                    if (!valid)
                    {
                        return valid;
                    }
                }
                if (admission)
                {
                    return owner_.validateFieldValue(lux::cxx::typeToken<Value>(), &before_, &value);
                }
                return {};
            }

            class Prepared final : public editing::PreparedEdit
            {
            public:
                explicit Prepared(const TFieldEdit& operation) : operation_(operation), commit_(AdoptExisting{}) {}

                Prepared(const TFieldEdit& operation, Value& live, const Value& next)
                    : operation_(operation), commit_(std::in_place_type<ReplaceValue>, live, next)
                {}

                editing::EEditEffect effect() const noexcept override
                {
                    return TFieldValue<Value>::equal(operation_.before_, operation_.after_)
                               ? editing::EEditEffect::NO_CHANGE
                               : editing::EEditEffect::CHANGE;
                }

            private:
                void apply() noexcept override
                {
                    std::visit([](auto& commit) noexcept { commit.apply(); }, commit_);
                    if (!operation_.after_protected_)
                    {
                        auto added = operation_.owner_.retainFieldTargets(
                            operation_.owner_.replayTarget(operation_.target_, operation_.identity_),
                            lux::cxx::typeToken<Component>()
                        );
                        for (auto& token : added)
                        {
                            operation_.protections_.push_back(std::move(token));
                        }
                        operation_.after_protected_ = true;
                    }
                }

                void publish(const editing::CommitInfo&) noexcept override
                {
                    operation_.owner_.fieldChanged(
                        operation_.owner_.replayTarget(operation_.target_, operation_.identity_),
                        lux::cxx::typeToken<Component>(),
                        false
                    );
                }

                struct AdoptExisting final
                {
                    // The validated value is already in the document. Only history and notice remain.
                    void apply() noexcept {}
                };
                struct ReplaceValue final
                {
                    ReplaceValue(Value& live, const Value& next) : live(live), next(next) {}

                    void apply() noexcept
                    {
                        TFieldValue<Value>::swap(live, next);
                    }

                    Value& live;
                    Value next;
                };

                const TFieldEdit& operation_;
                std::variant<AdoptExisting, ReplaceValue> commit_;
            };

            SceneEditing& owner_;
            SceneWriteTarget target_;
            lux::world::WorldObjectId identity_;
            std::string field_;
            std::string label_;
            Access access_;
            Value before_;
            Value after_{};
            mutable std::vector<lux::scene::PartitionRetention> protections_;
            mutable bool after_protected_{};
            bool already_applied_;
            bool captured_{};
        };
    } // namespace detail

    template <class Component, class Value, class Access>
    editing::EditResult<editing::ApplyResult> SceneEditing::setField(
        SceneWriteTarget target,
        std::string_view field,
        std::string_view label,
        Access access,
        const Value& value
    )
    {
        auto component = fieldAccess(target, lux::cxx::typeToken<Component>(), true);
        if (!component)
        {
            return lux::cxx::unexpected(component.error());
        }
        const auto* before = access(*static_cast<Component*>(*component));
        if (!before || field.empty())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        if (auto size = checkFieldSize(
                detail::addFieldBytes(TFieldValue<Value>::bytes(*before), TFieldValue<Value>::bytes(value))
            );
            !size)
        {
            return lux::cxx::unexpected(size.error());
        }
        editing::EditOperationPtr operation = std::make_unique<detail::TFieldEdit<Component, Value, Access>>(
            *this,
            target,
            field,
            label,
            std::move(access),
            *before,
            value,
            false
        );
        return executeField(operation);
    }

    template <class Component, class Value, class Access>
    editing::EditResult<FieldEditToken> SceneEditing::beginFieldEdit(
        SceneWriteTarget target,
        std::string origin,
        std::string_view field,
        std::string_view label,
        Access access
    )
    {
        auto component = fieldAccess(target, lux::cxx::typeToken<Component>(), true);
        if (!component)
        {
            return lux::cxx::unexpected(component.error());
        }
        const auto* before = access(*static_cast<Component*>(*component));
        if (!before || field.empty())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        if (auto size = checkFieldSize(TFieldValue<Value>::bytes(*before)); !size)
        {
            return lux::cxx::unexpected(size.error());
        }
        std::unique_ptr<detail::RegistryFieldEdit> operation =
            std::make_unique<detail::TFieldEdit<Component, Value, Access>>(
                *this,
                target,
                field,
                label,
                std::move(access),
                *before,
                *before,
                true
            );
        return adoptFieldEdit(std::move(origin), operation);
    }
} // namespace lux::editor::scene
