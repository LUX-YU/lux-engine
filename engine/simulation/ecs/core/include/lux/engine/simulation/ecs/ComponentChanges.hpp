#pragma once

#include <lux/engine/simulation/ecs/ChangeCursor.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/simulation/ecs/core/visibility.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace lux::simulation::ecs
{
    enum class EComponentChangeKind : std::uint8_t
    {
        ADDED,
        MODIFIED,
        REMOVED,
    };

    enum class EChangeReadStatus : std::uint8_t
    {
        CURRENT,
        RESYNC_REQUIRED,
    };

    struct ComponentChange final
    {
        Entity entity{NullEntity};
        EComponentChangeKind kind{EComponentChangeKind::MODIFIED};
    };

    namespace detail
    {
        class EcsChangeLog;

        struct ChangeRangeData final
        {
            const void* stream{};
            const void* block{};
            std::size_t block_offset{};
            std::uint64_t begin{};
            std::uint64_t end{};
            EChangeReadStatus status{EChangeReadStatus::CURRENT};
        };

        [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC ComponentChange
        componentChangeAt(
            const void* block,
            std::size_t block_offset,
            std::uint64_t sequence
        ) noexcept;

        LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC void advanceChangePosition(
            const void*& block,
            std::size_t& block_offset,
            std::uint64_t& sequence
        ) noexcept;

        LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC void releaseChangeStream(
            const void* stream
        ) noexcept;
    } // namespace detail

    template <class Component>
    class ComponentChanges final
    {
      public:
        class Iterator final
        {
          public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = ComponentChange;
            using difference_type = std::ptrdiff_t;

            Iterator() noexcept = default;

            [[nodiscard]] ComponentChange operator*() const noexcept
            {
                return detail::componentChangeAt(
                    block_, block_offset_, sequence_
                );
            }

            Iterator& operator++() noexcept
            {
                detail::advanceChangePosition(
                    block_, block_offset_, sequence_
                );
                return *this;
            }

            Iterator operator++(int) noexcept
            {
                Iterator copy = *this;
                ++*this;
                return copy;
            }

            [[nodiscard]] bool operator==(const Iterator& other) const noexcept
            {
                return sequence_ == other.sequence_;
            }

          private:
            Iterator(
                const void* block,
                std::size_t block_offset,
                std::uint64_t sequence
            ) noexcept
                : block_(block),
                  block_offset_(block_offset),
                  sequence_(sequence)
            {
            }

            const void* block_{};
            std::size_t block_offset_{};
            std::uint64_t sequence_{};

            friend class ComponentChanges;
        };

        ComponentChanges() noexcept = default;
        ComponentChanges(const ComponentChanges&) = delete;
        ComponentChanges& operator=(const ComponentChanges&) = delete;

        ComponentChanges(ComponentChanges&& other) noexcept
            : stream_(std::exchange(other.stream_, nullptr)),
              begin_block_(std::exchange(other.begin_block_, nullptr)),
              begin_block_offset_(
                  std::exchange(other.begin_block_offset_, 0)
              ),
              begin_(std::exchange(other.begin_, 0)),
              end_(std::exchange(other.end_, 0)),
              status_(other.status_)
        {
        }

        ComponentChanges& operator=(ComponentChanges&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                stream_ = std::exchange(other.stream_, nullptr);
                begin_block_ = std::exchange(other.begin_block_, nullptr);
                begin_block_offset_ = std::exchange(
                    other.begin_block_offset_, 0
                );
                begin_ = std::exchange(other.begin_, 0);
                end_ = std::exchange(other.end_, 0);
                status_ = other.status_;
            }
            return *this;
        }

        ~ComponentChanges() noexcept
        {
            reset();
        }

        [[nodiscard]] EChangeReadStatus status() const noexcept
        {
            return status_;
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return begin_ == end_;
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return static_cast<std::size_t>(end_ - begin_);
        }

        [[nodiscard]] Iterator begin() const noexcept
        {
            return Iterator(begin_block_, begin_block_offset_, begin_);
        }

        [[nodiscard]] Iterator end() const noexcept
        {
            return Iterator(nullptr, 0, end_);
        }

        [[nodiscard]] static ComponentChanges fromDetail(
            detail::ChangeRangeData data
        ) noexcept
        {
            return ComponentChanges(
                data.stream,
                data.block,
                data.block_offset,
                data.begin,
                data.end,
                data.status
            );
        }

      private:
        ComponentChanges(
            const void* stream,
            const void* block,
            std::size_t block_offset,
            std::uint64_t begin,
            std::uint64_t end,
            EChangeReadStatus status
        ) noexcept
            : stream_(stream),
              begin_block_(block),
              begin_block_offset_(block_offset),
              begin_(begin),
              end_(end),
              status_(status)
        {
        }

        void reset() noexcept
        {
            if (stream_ != nullptr)
                detail::releaseChangeStream(stream_);
            stream_ = nullptr;
            begin_block_ = nullptr;
            begin_block_offset_ = 0;
            begin_ = 0;
            end_ = 0;
        }

        const void* stream_{};
        const void* begin_block_{};
        std::size_t begin_block_offset_{};
        std::uint64_t begin_{};
        std::uint64_t end_{};
        EChangeReadStatus status_{EChangeReadStatus::CURRENT};

        friend class detail::EcsChangeLog;

    };
} // namespace lux::simulation::ecs
