#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <variant>

namespace lux::editor::scene
{
    namespace detail
    {
        inline std::size_t addFieldBytes(std::size_t first, std::size_t second) noexcept
        {
            constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
            return second > maximum - first ? maximum : first + second;
        }
    } // namespace detail

    // Typed value semantics, shared by generated GUI bindings and non-widget callers.
    // Specializations describe actual owned values; they do not own a document or a second model.
    template <class Value> struct FieldValue
    {
        static bool equal(const Value &first, const Value &second) noexcept
        {
            if constexpr (requires { first.coeffs(); })
            {
                return first.coeffs() == second.coeffs();
            }
            else if constexpr (lux::meta::HasTypeStaticInfo<Value>)
            {
                return std::apply(
                    [&](const auto &...field)
                    {
                        return (FieldValue<std::remove_cvref_t<decltype(first.*field.pointer)>>::equal(
                                    first.*field.pointer, second.*field.pointer) &&
                                ...);
                    },
                    lux::meta::TypeStaticInfo<Value>::fields);
            }
            else if constexpr (requires { std::variant_size<Value>::value; })
            {
                if (first.index() != second.index())
                {
                    return false;
                }
                return std::visit(
                    [](const auto &left, const auto &right)
                    {
                        using Item = std::remove_cvref_t<decltype(left)>;
                        if constexpr (std::same_as<Item, std::remove_cvref_t<decltype(right)>>)
                        {
                            return FieldValue<Item>::equal(left, right);
                        }
                        else
                        {
                            return false;
                        }
                    },
                    first, second);
            }
            else if constexpr (requires {
                                   first.has_value();
                                   *first;
                               })
            {
                return first.has_value() == second.has_value() &&
                       (!first.has_value() || FieldValue<typename Value::value_type>::equal(*first, *second));
            }
            else if constexpr (requires { std::tuple_size<Value>::value; })
            {
                return [&]<std::size_t... Index>(std::index_sequence<Index...>)
                {
                    return (FieldValue<std::remove_cvref_t<decltype(std::get<Index>(first))>>::equal(
                                std::get<Index>(first), std::get<Index>(second)) &&
                            ...);
                }(std::make_index_sequence<std::tuple_size_v<Value>>{});
            }
            else if constexpr (requires {
                                   typename Value::mapped_type;
                                   first.find(first.begin()->first);
                               })
            {
                if (first.size() != second.size())
                {
                    return false;
                }
                return std::ranges::all_of(first,
                                           [&](const auto &item)
                                           {
                                               const auto found = second.find(item.first);
                                               return found != second.end() &&
                                                      FieldValue<typename Value::mapped_type>::equal(item.second,
                                                                                                     found->second);
                                           });
            }
            else if constexpr (requires {
                                   typename Value::key_type;
                                   first.contains(*first.begin());
                               })
            {
                return first.size() == second.size() &&
                       std::ranges::all_of(first, [&](const auto &key) { return second.contains(key); });
            }
            else if constexpr (std::ranges::range<Value> && !requires { Value::SizeAtCompileTime; })
            {
                return std::ranges::equal(
                    first, second, [](const auto &left, const auto &right)
                    { return FieldValue<std::ranges::range_value_t<Value>>::equal(left, right); });
            }
            else
            {
                return first == second;
            }
        }

        static bool valid(const Value &value) noexcept
        {
            if constexpr (std::floating_point<Value>)
            {
                return std::isfinite(value);
            }
            else if constexpr (requires {
                                   value.w();
                                   value.coeffs();
                                   value.squaredNorm();
                               })
            {
                using Scalar = typename Value::Scalar;
                // Four products and their sum accumulate Scalar roundoff. Retain
                // the existing double tolerance without rejecting valid float rotations.
                constexpr double tolerance = (std::max)(1e-8, 16.0 * std::numeric_limits<Scalar>::epsilon());
                const double norm = value.coeffs().template cast<double>().squaredNorm();
                return value.coeffs().allFinite() && std::isfinite(norm) &&
                       norm > std::numeric_limits<Scalar>::epsilon() && std::abs(norm - 1.0) <= tolerance;
            }
            else if constexpr (requires { value.allFinite(); })
            {
                return value.allFinite();
            }
            else if constexpr (lux::meta::HasTypeStaticInfo<Value>)
            {
                return std::apply(
                    [&](const auto &...field)
                    {
                        return (FieldValue<std::remove_cvref_t<decltype(value.*field.pointer)>>::valid(value.*
                                                                                                       field.pointer) &&
                                ...);
                    },
                    lux::meta::TypeStaticInfo<Value>::fields);
            }
            else if constexpr (requires { std::variant_size<Value>::value; })
            {
                return !value.valueless_by_exception() &&
                       std::visit([](const auto &item)
                                  { return FieldValue<std::remove_cvref_t<decltype(item)>>::valid(item); }, value);
            }
            else if constexpr (requires {
                                   value.has_value();
                                   *value;
                               })
            {
                return !value.has_value() || FieldValue<typename Value::value_type>::valid(*value);
            }
            else if constexpr (requires { std::tuple_size<Value>::value; })
            {
                return std::apply([](const auto &...item)
                                  { return (FieldValue<std::remove_cvref_t<decltype(item)>>::valid(item) && ...); },
                                  value);
            }
            else if constexpr (std::ranges::range<Value>)
            {
                return std::ranges::all_of(value, [](const auto &item)
                                           { return FieldValue<std::ranges::range_value_t<Value>>::valid(item); });
            }
            else
            {
                return std::is_trivially_copyable_v<Value>;
            }
        }

        static std::size_t bytes(const Value &value) noexcept
        {
            std::size_t result = sizeof(Value);
            if constexpr (requires {
                              value.capacity();
                              typename Value::value_type;
                          })
            {
                const auto capacity = value.capacity();
                constexpr auto width = sizeof(typename Value::value_type);
                if (capacity > (std::numeric_limits<std::size_t>::max)() / width)
                {
                    return (std::numeric_limits<std::size_t>::max)();
                }
                result = detail::addFieldBytes(result, capacity * width);
            }
            if constexpr (lux::meta::HasTypeStaticInfo<Value>)
            {
                std::apply(
                    [&](const auto &...field)
                    {
                        ((result = detail::addFieldBytes(
                              result, FieldValue<std::remove_cvref_t<decltype(value.*field.pointer)>>::bytes(
                                          value.*field.pointer) -
                                          sizeof(value.*field.pointer))),
                         ...);
                    },
                    lux::meta::TypeStaticInfo<Value>::fields);
            }
            else if constexpr (requires { std::variant_size<Value>::value; })
            {
                if (!value.valueless_by_exception())
                {
                    result = detail::addFieldBytes(
                        result,
                        std::visit(
                            [](const auto &item)
                            { return FieldValue<std::remove_cvref_t<decltype(item)>>::bytes(item) - sizeof(item); },
                            value));
                }
            }
            else if constexpr (requires {
                                   value.has_value();
                                   *value;
                               })
            {
                if (value.has_value())
                {
                    result = detail::addFieldBytes(result, FieldValue<typename Value::value_type>::bytes(*value) -
                                                               sizeof(*value));
                }
            }
            else if constexpr (std::ranges::range<Value> && !requires { Value::SizeAtCompileTime; })
            {
                for (const auto &item : value)
                {
                    // Non-contiguous containers also charge node/link storage per retained element.
                    const auto storage = []
                    {
                        if constexpr (requires { std::declval<Value>().capacity(); })
                        {
                            return std::size_t{};
                        }
                        else if constexpr (requires { std::tuple_size<Value>::value; })
                        {
                            return std::size_t{};
                        }
                        else
                        {
                            return sizeof(item) + 4 * sizeof(void *);
                        }
                    }();
                    const auto payload = FieldValue<std::remove_cvref_t<decltype(item)>>::bytes(item) - sizeof(item);
                    result = detail::addFieldBytes(result, detail::addFieldBytes(storage, payload));
                }
                if constexpr (requires { value.bucket_count(); })
                {
                    result = detail::addFieldBytes(result, value.bucket_count() * 2 * sizeof(void *));
                }
            }
            else if constexpr (requires { std::tuple_size<Value>::value; })
            {
                std::apply(
                    [&](const auto &...item)
                    {
                        ((result = detail::addFieldBytes(
                              result, FieldValue<std::remove_cvref_t<decltype(item)>>::bytes(item) - sizeof(item))),
                         ...);
                    },
                    value);
            }
            return result;
        }

        static void swap(Value &first, Value &second) noexcept
        {
            if constexpr (requires {
                              Value::SizeAtCompileTime;
                              first.data();
                          })
            {
                static_assert(Value::SizeAtCompileTime >= 0, "Dynamic matrices require a typed edit binding");
                for (std::ptrdiff_t index = 0; index < Value::SizeAtCompileTime; ++index)
                {
                    std::swap(first.data()[index], second.data()[index]);
                }
            }
            else if constexpr (requires { first.coeffs(); })
            {
                for (std::ptrdiff_t index = 0; index < 4; ++index)
                {
                    std::swap(first.coeffs()[index], second.coeffs()[index]);
                }
            }
            else
            {
                static_assert(std::is_nothrow_swappable_v<Value>);
                using std::swap;
                swap(first, second);
            }
        }
    };

    namespace detail
    {
        template <class Component, class Value, class Access> class FieldEdit final : public SceneFieldEdit
        {
          public:
            FieldEdit(SceneEditor &owner, SceneWriteTarget target, std::string_view field, std::string_view label,
                      Access access, const Value &before, const Value &after, bool preview)
                : owner_(owner), target_(target), field_(field), label_(label), access_(std::move(access)),
                  before_(before), after_(after), preview_(preview)
            {
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
                    sizeof(*this) + field_.capacity() + label_.capacity() + 2,
                    addFieldBytes(FieldValue<Value>::bytes(before_), FieldValue<Value>::bytes(after_)));
            }

            editing::EditResult<editing::PreparedEditPtr> prepare(
                const editing::ApplyContext &context, editing::EditPreparationBudget &budget) const noexcept override
            {
                auto live = locate(false);
                if (!live)
                {
                    return lux::cxx::unexpected(live.error());
                }
                const bool backwards = context.direction == editing::EDirection::BACKWARD;
                const bool preview_commit = preview_ && context.kind == editing::EApplyKind::EXECUTE;
                const auto &expected = backwards || preview_commit ? after_ : before_;
                const auto &next = backwards ? before_ : after_;
                if (!FieldValue<Value>::equal(**live, expected))
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
                }
                if (auto valid = validate(next, context.kind == editing::EApplyKind::EXECUTE); !valid)
                {
                    return lux::cxx::unexpected(valid.error());
                }
                const auto staging =
                    preview_commit ? sizeof(Prepared) : sizeof(Prepared) + FieldValue<Value>::bytes(next);
                if (auto reserved = budget.reserve(staging); !reserved)
                {
                    return lux::cxx::unexpected(reserved.error());
                }
                if (preview_commit)
                {
                    return editing::PreparedEditPtr{new Prepared(*this)};
                }
                return editing::PreparedEditPtr{new Prepared(*this, **live, next)};
            }

            editing::EditResult<void> update(lux::cxx::TypeToken type, const void *value) override
            {
                if (type != lux::cxx::typeToken<Value>())
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
                }
                const auto &next = *static_cast<const Value *>(value);
                if (auto valid = validate(next, true); !valid)
                {
                    return valid;
                }
                if (auto size = owner_.checkFieldSize(FieldValue<Value>::bytes(next)); !size)
                {
                    return size;
                }
                auto live = locate(false);
                if (!live)
                {
                    return lux::cxx::unexpected(live.error());
                }
                if (!FieldValue<Value>::equal(**live, after_))
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
                }
                Value prepared(next);
                Value retained(next);
                FieldValue<Value>::swap(**live, prepared);
                FieldValue<Value>::swap(after_, retained);
                owner_.fieldChanged(target_, lux::cxx::typeToken<Component>(), true);
                return {};
            }

            editing::EditResult<void> cancel() noexcept override
            {
                auto live = locate(false);
                if (!live)
                {
                    return lux::cxx::unexpected(live.error());
                }
                if (!FieldValue<Value>::equal(**live, after_))
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
                }
                FieldValue<Value>::swap(**live, before_);
                owner_.fieldChanged(target_, lux::cxx::typeToken<Component>(), true);
                return {};
            }

          private:
            editing::EditResult<Value *> locate(bool current) const
            {
                auto component = owner_.fieldAccess(target_, lux::cxx::typeToken<Component>(), current);
                if (!component)
                {
                    return lux::cxx::unexpected(component.error());
                }
                auto *value = access_(*static_cast<Component *>(*component));
                if (!value)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
                }
                return value;
            }

            editing::EditResult<void> validate(const Value &value, bool admission) const
            {
                if (!FieldValue<Value>::valid(value))
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
                explicit Prepared(const FieldEdit &operation) : operation_(operation), commit_(AdoptPreview{}) {}

                Prepared(const FieldEdit &operation, Value &live, const Value &next)
                    : operation_(operation), commit_(std::in_place_type<ReplaceValue>, live, next)
                {
                }

                editing::EEditEffect effect() const noexcept override
                {
                    return FieldValue<Value>::equal(operation_.before_, operation_.after_)
                               ? editing::EEditEffect::NO_CHANGE
                               : editing::EEditEffect::CHANGE;
                }

              private:
                void apply() noexcept override
                {
                    std::visit([](auto &commit) noexcept { commit.apply(); }, commit_);
                }

                void publish(const editing::CommitInfo &) noexcept override
                {
                    operation_.owner_.fieldChanged(operation_.target_, lux::cxx::typeToken<Component>(), false);
                }

                struct AdoptPreview final
                {
                    // The validated value is already in the document. Only history and notice remain.
                    void apply() noexcept {}
                };
                struct ReplaceValue final
                {
                    ReplaceValue(Value &live, const Value &next) : live(live), next(next) {}

                    void apply() noexcept
                    {
                        FieldValue<Value>::swap(live, next);
                    }

                    Value &live;
                    Value next;
                };

                const FieldEdit &operation_;
                std::variant<AdoptPreview, ReplaceValue> commit_;
            };

            SceneEditor &owner_;
            SceneWriteTarget target_;
            std::string field_;
            std::string label_;
            Access access_;
            Value before_;
            Value after_;
            bool preview_;
        };
    } // namespace detail

    template <class Component, class Value, class Access>
    editing::EditResult<editing::ApplyResult> SceneEditor::setField(SceneWriteTarget target, std::string_view field,
                                                                    std::string_view label, Access access,
                                                                    const Value &value)
    {
        auto component = fieldAccess(target, lux::cxx::typeToken<Component>(), true);
        if (!component)
        {
            return lux::cxx::unexpected(component.error());
        }
        const auto *before = access(*static_cast<Component *>(*component));
        if (!before || field.empty())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        if (auto size = checkFieldSize(
                detail::addFieldBytes(FieldValue<Value>::bytes(*before), FieldValue<Value>::bytes(value)));
            !size)
        {
            return lux::cxx::unexpected(size.error());
        }
        editing::EditOperationPtr operation = std::make_unique<detail::FieldEdit<Component, Value, Access>>(
            *this, target, field, label, std::move(access), *before, value, false);
        return executeField(operation);
    }

    template <class Component, class Value, class Access>
    editing::EditResult<PreviewToken> SceneEditor::beginPreview(SceneWriteTarget target, std::string origin,
                                                                std::string_view field, std::string_view label,
                                                                Access access)
    {
        auto component = fieldAccess(target, lux::cxx::typeToken<Component>(), true);
        if (!component)
        {
            return lux::cxx::unexpected(component.error());
        }
        const auto *before = access(*static_cast<Component *>(*component));
        if (!before || field.empty())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        if (auto size = checkFieldSize(FieldValue<Value>::bytes(*before)); !size)
        {
            return lux::cxx::unexpected(size.error());
        }
        std::unique_ptr<detail::SceneFieldEdit> operation =
            std::make_unique<detail::FieldEdit<Component, Value, Access>>(*this, target, field, label,
                                                                          std::move(access), *before, *before, true);
        return adoptPreview(std::move(origin), operation);
    }
} // namespace lux::editor::scene
