#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/snapshot/detail/ComponentSnapshotAccess.hpp>

#include <array>
#include <concepts>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    class ComponentSnapshotSet;
    class ComponentSnapshotBinding;
    class WorldSnapshot;

    namespace detail
    {
        struct ComponentSnapshotSetAccess;

        using CloneComponentStorageFn = void (*)(
            const World&,
            WorldEdit&
        );

#if defined(LUX_ECS_SNAPSHOT_TESTING)
        struct ComponentSnapshotTestStats final
        {
            static inline std::size_t clone_calls{};
            static inline std::size_t storage_lookups{};

            static void reset() noexcept
            {
                clone_calls = 0U;
                storage_lookups = 0U;
            }
        };
#endif

        template <class Component>
        void cloneComponentStorage(const World& source, WorldEdit& target)
        {
#if defined(LUX_ECS_SNAPSHOT_TESTING)
            ++ComponentSnapshotTestStats::clone_calls;
            ++ComponentSnapshotTestStats::storage_lookups;
#endif
            const auto* source_storage =
                ComponentSnapshotStorageAccess::storage<Component>(source);
            if (source_storage == nullptr || source_storage->empty())
                return;

            auto& target_storage =
                ComponentSnapshotStorageAccess::storage<Component>(target);
            target_storage.reserve(source_storage->size());
            auto entities = source_storage->each();
            using Iterator = decltype(entities.begin());
            const StorageEntityIterator<Iterator> first(entities.begin());
            const StorageEntityIterator<Iterator> last(entities.end());
            if constexpr (std::is_empty_v<Component>)
            {
                target_storage.insert(first, last);
            }
            else
            {
                target_storage.insert(
                    first,
                    last,
                    source_storage->begin()
                );
            }
        }
    } // namespace detail

    class ComponentSnapshotBinding final
    {
      public:
        [[nodiscard]] constexpr const ComponentSchema& schema() const noexcept
        {
            return *schema_;
        }

      private:
        constexpr ComponentSnapshotBinding(
            const ComponentSchema& schema,
            detail::CloneComponentStorageFn clone
        ) noexcept
            : schema_(&schema), clone_(clone)
        {
        }

        template <class Component>
        friend constexpr ComponentSnapshotBinding bindComponentSnapshot(
            const ComponentSchema&
        ) noexcept;
        friend class ComponentSnapshotSet;
        friend class WorldSnapshot;
        friend struct detail::ComponentSnapshotSetAccess;

        const ComponentSchema* schema_{};
        detail::CloneComponentStorageFn clone_{};
    };

    namespace detail
    {
        template <class... Binding>
            requires (std::same_as<
                std::remove_cvref_t<Binding>,
                ComponentSnapshotBinding> && ...)
        [[nodiscard]] constexpr auto componentSnapshotBindings(
            Binding&&... binding
        ) noexcept
        {
            return std::array<ComponentSnapshotBinding, sizeof...(Binding)>{
                std::forward<Binding>(binding)...};
        }
    } // namespace detail

    struct ComponentSnapshotContribution final
    {
        std::shared_ptr<const void> code_lifetime;
        std::span<const ComponentSnapshotBinding> bindings;
    };

    template <class Component>
    [[nodiscard]] constexpr ComponentSnapshotBinding bindComponentSnapshot(
        const ComponentSchema& schema
    ) noexcept
    {
        static_assert(std::copy_constructible<Component>);
        return ComponentSnapshotBinding{
            schema,
            &detail::cloneComponentStorage<Component>
        };
    }
} // namespace lux::ecs
