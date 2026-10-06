#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
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
    template <class Value> struct TFieldValue
    {
        static bool equal(const Value& first, const Value& second) noexcept
        {
            if constexpr (requires { first.coeffs(); })
            {
                return first.coeffs() == second.coeffs();
            }
            else if constexpr (lux::meta::HasTypeStaticInfo<Value>)
            {
                return std::apply(
                    [&](const auto&... field) {
                        return (
                            TFieldValue<std::remove_cvref_t<decltype(first.*field.pointer)>>::equal(
                                first.*field.pointer,
                                second.*field.pointer
                            ) &&
                            ...
                        );
                    },
                    lux::meta::TTypeStaticInfo<Value>::fields
                );
            }
            else if constexpr (requires { std::variant_size<Value>::value; })
            {
                if (first.index() != second.index())
                {
                    return false;
                }
                return std::visit(
                    [](const auto& left, const auto& right) {
                        using Item = std::remove_cvref_t<decltype(left)>;
                        if constexpr (std::same_as<Item, std::remove_cvref_t<decltype(right)>>)
                        {
                            return TFieldValue<Item>::equal(left, right);
                        }
                        else
                        {
                            return false;
                        }
                    },
                    first,
                    second
                );
            }
            else if constexpr (requires {
                                   first.has_value();
                                   *first;
                               })
            {
                return first.has_value() == second.has_value() &&
                       (!first.has_value() || TFieldValue<typename Value::value_type>::equal(*first, *second));
            }
            else if constexpr (requires { std::tuple_size<Value>::value; })
            {
                return [&]<std::size_t... Index>(std::index_sequence<Index...>) {
                    return (
                        TFieldValue<std::remove_cvref_t<decltype(std::get<Index>(first))>>::equal(
                            std::get<Index>(first),
                            std::get<Index>(second)
                        ) &&
                        ...
                    );
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
                return std::ranges::all_of(first, [&](const auto& item) {
                    const auto found = second.find(item.first);
                    return found != second.end() &&
                           TFieldValue<typename Value::mapped_type>::equal(item.second, found->second);
                });
            }
            else if constexpr (requires {
                                   typename Value::key_type;
                                   first.contains(*first.begin());
                               })
            {
                return first.size() == second.size() &&
                       std::ranges::all_of(first, [&](const auto& key) { return second.contains(key); });
            }
            else if constexpr (std::ranges::range<Value> && !requires { Value::SizeAtCompileTime; })
            {
                return std::ranges::equal(first, second, [](const auto& left, const auto& right) {
                    return TFieldValue<std::ranges::range_value_t<Value>>::equal(left, right);
                });
            }
            else
            {
                return first == second;
            }
        }

        static bool valid(const Value& value) noexcept
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
                    [&](const auto&... field) {
                        return (
                            TFieldValue<std::remove_cvref_t<decltype(value.*field.pointer)>>::valid(
                                value.*field.pointer
                            ) &&
                            ...
                        );
                    },
                    lux::meta::TTypeStaticInfo<Value>::fields
                );
            }
            else if constexpr (requires { std::variant_size<Value>::value; })
            {
                return !value.valueless_by_exception() &&
                       std::visit(
                           [](const auto& item) {
                               return TFieldValue<std::remove_cvref_t<decltype(item)>>::valid(item);
                           },
                           value
                       );
            }
            else if constexpr (requires {
                                   value.has_value();
                                   *value;
                               })
            {
                return !value.has_value() || TFieldValue<typename Value::value_type>::valid(*value);
            }
            else if constexpr (requires { std::tuple_size<Value>::value; })
            {
                return std::apply(
                    [](const auto&... item) {
                        return (TFieldValue<std::remove_cvref_t<decltype(item)>>::valid(item) && ...);
                    },
                    value
                );
            }
            else if constexpr (std::ranges::range<Value>)
            {
                return std::ranges::all_of(value, [](const auto& item) {
                    return TFieldValue<std::ranges::range_value_t<Value>>::valid(item);
                });
            }
            else
            {
                return std::is_trivially_copyable_v<Value>;
            }
        }

        static std::size_t bytes(const Value& value) noexcept
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
                    [&](const auto&... field) {
                        ((result = detail::addFieldBytes(
                              result,
                              TFieldValue<std::remove_cvref_t<decltype(value.*field.pointer)>>::bytes(
                                  value.*field.pointer
                              ) - sizeof(value.*field.pointer)
                          )),
                         ...);
                    },
                    lux::meta::TTypeStaticInfo<Value>::fields
                );
            }
            else if constexpr (requires { std::variant_size<Value>::value; })
            {
                if (!value.valueless_by_exception())
                {
                    result = detail::addFieldBytes(
                        result,
                        std::visit(
                            [](const auto& item) {
                                return TFieldValue<std::remove_cvref_t<decltype(item)>>::bytes(item) - sizeof(item);
                            },
                            value
                        )
                    );
                }
            }
            else if constexpr (requires {
                                   value.has_value();
                                   *value;
                               })
            {
                if (value.has_value())
                {
                    result = detail::addFieldBytes(
                        result,
                        TFieldValue<typename Value::value_type>::bytes(*value) - sizeof(*value)
                    );
                }
            }
            else if constexpr (std::ranges::range<Value> && !requires { Value::SizeAtCompileTime; })
            {
                for (const auto& item : value)
                {
                    // Non-contiguous containers also charge node/link storage per retained element.
                    const auto storage = [] {
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
                            return sizeof(item) + 4 * sizeof(void*);
                        }
                    }();
                    const auto payload = TFieldValue<std::remove_cvref_t<decltype(item)>>::bytes(item) - sizeof(item);
                    result = detail::addFieldBytes(result, detail::addFieldBytes(storage, payload));
                }
                if constexpr (requires { value.bucket_count(); })
                {
                    result = detail::addFieldBytes(result, value.bucket_count() * 2 * sizeof(void*));
                }
            }
            else if constexpr (requires { std::tuple_size<Value>::value; })
            {
                std::apply(
                    [&](const auto&... item) {
                        ((result = detail::addFieldBytes(
                              result,
                              TFieldValue<std::remove_cvref_t<decltype(item)>>::bytes(item) - sizeof(item)
                          )),
                         ...);
                    },
                    value
                );
            }
            return result;
        }

        static void swap(Value& first, Value& second) noexcept
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

}
