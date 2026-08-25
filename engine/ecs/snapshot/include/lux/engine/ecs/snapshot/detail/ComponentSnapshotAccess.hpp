#pragma once

#include <lux/engine/ecs/World.hpp>

#include <iterator>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lux::ecs::detail
{
    template <class Iterator>
    class StorageEntityIterator final
    {
      public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Entity;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = Entity;

        StorageEntityIterator() = default;

        explicit StorageEntityIterator(Iterator iterator)
            : iterator_(std::move(iterator))
        {
        }

        [[nodiscard]] Entity operator*() const noexcept
        {
            return std::get<0>(*iterator_);
        }

        StorageEntityIterator& operator++() noexcept
        {
            ++iterator_;
            return *this;
        }

        void operator++(int) noexcept
        {
            ++*this;
        }

        [[nodiscard]] bool operator==(
            const StorageEntityIterator& other
        ) const noexcept
        {
            return iterator_ == other.iterator_;
        }

      private:
        Iterator iterator_;
    };

    struct ComponentSnapshotStorageAccess final
    {
        template <class Component>
        [[nodiscard]] static const auto* storage(
            const World& world
        ) noexcept
        {
            return world.registry_.template storage<Component>();
        }

        template <class Component>
        [[nodiscard]] static auto& storage(WorldEdit& edit)
        {
            require(edit.world_ != nullptr);
            return edit.world_->registry_.template storage<Component>();
        }
    };
} // namespace lux::ecs::detail
