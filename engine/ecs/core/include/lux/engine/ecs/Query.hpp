#pragma once

#include <lux/engine/ecs/ComponentChanges.hpp>
#include <lux/engine/ecs/Entity.hpp>

#include <entt/core/type_info.hpp>

#include <concepts>
#include <array>
#include <cstdint>
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

        struct BoundWorldChangeStream final
        {
            void* owner{};
            void* stream{};
            void (*append)(
                void*,
                void*,
                Entity,
                EComponentChangeKind
            ) noexcept{};

            void operator()(
                Entity entity,
                EComponentChangeKind kind
            ) const noexcept
            {
                if (append != nullptr)
                    append(owner, stream, entity, kind);
            }

            [[nodiscard]] explicit operator bool() const noexcept
            {
                return append != nullptr;
            }
        };

        struct ChangeStreamBinder final
        {
            void* context{};
            BoundWorldChangeStream (*bind)(void*, std::uint64_t) noexcept{};

            [[nodiscard]] BoundWorldChangeStream operator()(
                std::uint64_t storage
            ) const noexcept
            {
                return bind == nullptr
                    ? BoundWorldChangeStream{}
                    : bind(context, storage);
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
                std::is_const_v<Registry> || !AccessTraits<Value>::kWrite,
                const Component<Value>,
                Component<Value>>;

            using View = decltype(
                std::declval<Registry&>().template view<ViewComponent<Access>...>()
            );
            using BaseIterator = decltype(
                std::declval<View&>().each().begin()
            );

          public:
            class Iterator final
            {
              public:
                Iterator() = default;

                Iterator& operator++()
                {
                    ++iterator_;
                    recorded_current_ = false;
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
                    auto result = *iterator_;
                    const Entity entity = std::get<0>(result);
                    if (!recorded_current_)
                    {
                        recordWrites(
                            entity,
                            std::index_sequence_for<Access...>{}
                        );
                        recorded_current_ = true;
                    }
                    return result;
                }

              private:
                Iterator(
                    BaseIterator iterator,
                    const std::array<
                        BoundWorldChangeStream,
                        sizeof...(Access)
                    >& streams,
                    ChangeRecorder recorder
                ) noexcept
                    : iterator_(iterator),
                      streams_(streams),
                      recorder_(recorder)
                {
                }

                template <std::size_t Index, class Value>
                void recordWrite(Entity entity) const noexcept
                {
                    if constexpr (AccessTraits<Value>::kWrite)
                    {
                        const auto& stream = streams_[Index];
                        if (stream)
                            stream(entity, EComponentChangeKind::MODIFIED);
                        else
                        {
                            recorder_(
                                entt::type_hash<Component<Value>>::value(),
                                entity,
                                EComponentChangeKind::MODIFIED
                            );
                        }
                    }
                }

                template <std::size_t... Index>
                void recordWrites(
                    Entity entity,
                    std::index_sequence<Index...>
                ) const noexcept
                {
                    (recordWrite<Index, Access>(entity), ...);
                }

                BaseIterator iterator_{};
                std::array<BoundWorldChangeStream, sizeof...(Access)> streams_{};
                ChangeRecorder recorder_{};
                mutable bool recorded_current_{};

                friend class BasicQuery;
            };

            BasicQuery(
                Registry& registry,
                ChangeStreamBinder binder = {},
                ChangeRecorder recorder = {}
            )
                : view_(registry.template view<ViewComponent<Access>...>()),
                  streams_{bindStream<Access>(binder)...},
                  recorder_(recorder)
            {
                static_assert(sizeof...(Access) != 0);
                static_assert(uniqueComponents<Access...>());
                if constexpr (std::is_const_v<Registry>)
                    static_assert((!AccessTraits<Access>::kWrite && ...));
            }

            [[nodiscard]] Iterator begin()
            {
                return Iterator(view_.each().begin(), streams_, recorder_);
            }

            [[nodiscard]] Iterator end()
            {
                return Iterator(view_.each().end(), streams_, recorder_);
            }

          private:
            template <class Value>
            [[nodiscard]] static BoundWorldChangeStream bindStream(
                ChangeStreamBinder binder
            ) noexcept
            {
                if constexpr (AccessTraits<Value>::kWrite)
                {
                    return binder(
                        entt::type_hash<Component<Value>>::value()
                    );
                }
                else
                    return {};
            }

            View view_;
            std::array<BoundWorldChangeStream, sizeof...(Access)> streams_{};
            ChangeRecorder recorder_{};
        };
    } // namespace detail
} // namespace lux::ecs
