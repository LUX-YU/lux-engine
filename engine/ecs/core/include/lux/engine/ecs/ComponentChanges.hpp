#pragma once

#include <lux/engine/ecs/ChangeCursor.hpp>
#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/ecs/core/visibility.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace lux::ecs
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
        class ChangeJournal;

        struct ChangeRangeData final
        {
            const void* stream{};
            std::uint64_t begin{};
            std::uint64_t end{};
            EChangeReadStatus status{EChangeReadStatus::CURRENT};
        };

        [[nodiscard]] LUX_ENGINE_ECS_CORE_PUBLIC ComponentChange
        componentChangeAt(const void* stream, std::uint64_t sequence) noexcept;

        LUX_ENGINE_ECS_CORE_PUBLIC void releaseChangeStream(
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
                return detail::componentChangeAt(stream_, sequence_);
            }

            Iterator& operator++() noexcept
            {
                ++sequence_;
                return *this;
            }

            Iterator operator++(int) noexcept
            {
                Iterator copy = *this;
                ++*this;
                return copy;
            }

            [[nodiscard]] bool operator==(const Iterator&) const noexcept = default;

          private:
            Iterator(const void* stream, std::uint64_t sequence) noexcept
                : stream_(stream), sequence_(sequence)
            {
            }

            const void* stream_{};
            std::uint64_t sequence_{};

            friend class ComponentChanges;
        };

        ComponentChanges() noexcept = default;
        ComponentChanges(const ComponentChanges&) = delete;
        ComponentChanges& operator=(const ComponentChanges&) = delete;

        ComponentChanges(ComponentChanges&& other) noexcept
            : stream_(std::exchange(other.stream_, nullptr)),
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
            return Iterator(stream_, begin_);
        }

        [[nodiscard]] Iterator end() const noexcept
        {
            return Iterator(stream_, end_);
        }

        [[nodiscard]] static ComponentChanges fromDetail(
            detail::ChangeRangeData data
        ) noexcept
        {
            return ComponentChanges(
                data.stream,
                data.begin,
                data.end,
                data.status
            );
        }

      private:
        ComponentChanges(
            const void* stream,
            std::uint64_t begin,
            std::uint64_t end,
            EChangeReadStatus status
        ) noexcept
            : stream_(stream), begin_(begin), end_(end), status_(status)
        {
        }

        void reset() noexcept
        {
            if (stream_ != nullptr)
                detail::releaseChangeStream(stream_);
            stream_ = nullptr;
            begin_ = 0;
            end_ = 0;
        }

        const void* stream_{};
        std::uint64_t begin_{};
        std::uint64_t end_{};
        EChangeReadStatus status_{EChangeReadStatus::CURRENT};

        friend class detail::ChangeJournal;

    };
} // namespace lux::ecs
