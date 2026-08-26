#pragma once

#include <lux/engine/simulation/ecs/EcsState.hpp>

#include <iterator>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lux::simulation::ecs::detail
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

} // namespace lux::simulation::ecs::detail
