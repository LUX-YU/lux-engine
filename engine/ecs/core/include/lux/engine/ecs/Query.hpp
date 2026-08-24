#pragma once

#include <lux/engine/ecs/ComponentChanges.hpp>
#include <lux/engine/ecs/Entity.hpp>

#include <entt/core/type_info.hpp>

#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    template <class Component>
    struct Read final
    {
        using component_type = Component;
    };

    template <class Component>
    struct Write final
    {
        using component_type = Component;
    };

    template <class... Access>
    struct QuerySpec final
    {
    };

    template <class... Access>
    [[nodiscard]] consteval QuerySpec<Access...> query() noexcept
    {
        return {};
    }

    namespace detail
    {
        template <class Access>
        struct AccessTraits;

        template <class Component>
        struct AccessTraits<Read<Component>> final
        {
            using ComponentType = Component;
            static constexpr bool kWrite = false;
        };

        template <class Component>
        struct AccessTraits<Write<Component>> final
        {
            using ComponentType = Component;
            static constexpr bool kWrite = true;
        };

        template <class Access>
        concept ComponentAccessSpec = requires
        {
            typename AccessTraits<Access>::ComponentType;
        };

        struct ChangeRecorder final
        {
            void* context{};
            void (*record)(
                void*,
                std::uint64_t,
                Entity,
                EComponentChangeKind
            ) noexcept{};

            void operator()(
                std::uint64_t storage,
                Entity entity,
                EComponentChangeKind kind
            ) const noexcept
            {
                if (record != nullptr)
                    record(context, storage, entity, kind);
            }
        };

        template <class... Access>
        [[nodiscard]] consteval bool uniqueComponents() noexcept
        {
            return []<std::size_t... Index>(std::index_sequence<Index...>)
            {
                using Values = std::tuple<
                    typename AccessTraits<Access>::ComponentType...>;
                return (([]<std::size_t Current, class Tuple>()
                {
                    using Value = std::tuple_element_t<Current, Tuple>;
                    return []<std::size_t... Previous>(
                        std::index_sequence<Previous...>)
                    {
                        return (!std::same_as<
                            Value,
                            std::tuple_element_t<Previous, Tuple>> && ...);
                    }(std::make_index_sequence<Current>{});
                }.template operator()<Index, Values>()) && ...);
            }(std::index_sequence_for<Access...>{});
        }

        template <class Registry, class... Access>
            requires (ComponentAccessSpec<Access> && ...)
        class BasicQuery final
        {
          private:
            template <class Value>
            using Component = typename AccessTraits<Value>::ComponentType;

            template <class Value>
            using ViewComponent = std::conditional_t<
                std::is_const_v<Registry>,
                const Component<Value>,
                Component<Value>>;

            using View = decltype(
                std::declval<Registry&>().template view<ViewComponent<Access>...>()
            );
            using BaseIterator = decltype(std::declval<View&>().begin());

            template <class Value>
            using Reference = std::conditional_t<
                AccessTraits<Value>::kWrite,
                Component<Value>&,
                const Component<Value>&>;

          public:
            class Iterator final
            {
              public:
                Iterator() = default;

                Iterator& operator++()
                {
                    ++iterator_;
                    return *this;
                }

                Iterator operator++(int)
                {
                    Iterator copy = *this;
                    ++*this;
                    return copy;
                }

                [[nodiscard]] bool operator==(const Iterator& other) const
                {
                    return iterator_ == other.iterator_;
                }

                [[nodiscard]] auto operator*() const
                {
                    const Entity entity = *iterator_;
                    (recordWrite<Access>(entity), ...);
                    return std::tuple<Entity, Reference<Access>...>{
                        entity,
                        reference<Access>(entity)...};
                }

              private:
                Iterator(
                    Registry& registry,
                    BaseIterator iterator,
                    ChangeRecorder recorder
                ) noexcept
                    : registry_(std::addressof(registry)),
                      iterator_(iterator),
                      recorder_(recorder)
                {
                }

                template <class Value>
                void recordWrite(Entity entity) const noexcept
                {
                    if constexpr (AccessTraits<Value>::kWrite)
                    {
                        recorder_(
                            entt::type_hash<Component<Value>>::value(),
                            entity,
                            EComponentChangeKind::MODIFIED
                        );
                    }
                }

                template <class Value>
                [[nodiscard]] Reference<Value> reference(Entity entity) const
                {
                    auto& value = registry_->template get<ViewComponent<Value>>(entity);
                    if constexpr (AccessTraits<Value>::kWrite)
                        return value;
                    else
                        return std::as_const(value);
                }

                Registry* registry_{};
                BaseIterator iterator_{};
                ChangeRecorder recorder_{};

                friend class BasicQuery;
            };

            BasicQuery(Registry& registry, ChangeRecorder recorder = {})
                : registry_(std::addressof(registry)),
                  view_(registry.template view<ViewComponent<Access>...>()),
                  recorder_(recorder)
            {
                static_assert(sizeof...(Access) != 0);
                static_assert(uniqueComponents<Access...>());
                if constexpr (std::is_const_v<Registry>)
                    static_assert((!AccessTraits<Access>::kWrite && ...));
            }

            [[nodiscard]] Iterator begin()
            {
                return Iterator(*registry_, view_.begin(), recorder_);
            }

            [[nodiscard]] Iterator end()
            {
                return Iterator(*registry_, view_.end(), recorder_);
            }

          private:
            Registry* registry_{};
            View view_;
            ChangeRecorder recorder_{};
        };
    } // namespace detail
} // namespace lux::ecs
