#pragma once

#include <lux/engine/ecs/ComponentChanges.hpp>
#include <lux/engine/ecs/Parent.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/hierarchy/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>

namespace lux::ecs
{
    class SystemContext;
    class SystemStart;

    namespace detail
    {
        struct HierarchyIndexTestAccess;
    }

    enum class EHierarchyError : std::uint8_t
    {
        NONE,
        INVALID_ENTITY,
        SELF_PARENT,
        CYCLE,
        INVALID_PARENT,
        NOT_SYNCHRONIZED,
        ALLOCATION_FAILURE,
    };

    enum class EHierarchyChangeKind : std::uint8_t
    {
        ATTACHED,
        REPARENTED,
        DETACHED,
    };

    struct HierarchyChange final
    {
        Entity entity{NullEntity};
        Entity previous_parent{NullEntity};
        Entity parent{NullEntity};
        EHierarchyChangeKind kind{EHierarchyChangeKind::DETACHED};
    };

    class HierarchyIndex;
    class HierarchySystem;

    class HierarchyChangeCursor final
    {
      public:
        HierarchyChangeCursor() noexcept = default;

      private:
        std::uint64_t epoch_{};
        std::uint64_t sequence_{};

        friend class HierarchyIndex;
    };

    class LUX_ENGINE_ECS_HIERARCHY_PUBLIC HierarchyChildren final
    {
      public:
        class LUX_ENGINE_ECS_HIERARCHY_PUBLIC Iterator final
        {
          public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = Entity;
            using difference_type = std::ptrdiff_t;

            Iterator() noexcept = default;

            [[nodiscard]] Entity operator*() const noexcept
            {
                return entity_;
            }

            Iterator& operator++() noexcept;

            Iterator operator++(int) noexcept
            {
                Iterator result = *this;
                ++*this;
                return result;
            }

            [[nodiscard]] bool operator==(const Iterator&) const noexcept =
                default;

          private:
            Iterator(
                const HierarchyIndex* hierarchy,
                Entity entity
            ) noexcept
                : hierarchy_(hierarchy), entity_(entity)
            {
            }

            const HierarchyIndex* hierarchy_{};
            Entity entity_{NullEntity};

            friend class HierarchyChildren;
        };

        [[nodiscard]] Iterator begin() const noexcept;
        [[nodiscard]] Iterator end() const noexcept;
        [[nodiscard]] bool empty() const noexcept;

      private:
        HierarchyChildren(
            const HierarchyIndex& hierarchy,
            Entity parent
        ) noexcept
            : hierarchy_(std::addressof(hierarchy)), parent_(parent)
        {
        }

        const HierarchyIndex* hierarchy_{};
        Entity parent_{NullEntity};

        friend class HierarchyIndex;
    };

    class LUX_ENGINE_ECS_HIERARCHY_PUBLIC HierarchyChanges final
    {
      public:
        class LUX_ENGINE_ECS_HIERARCHY_PUBLIC Iterator final
        {
          public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = HierarchyChange;
            using difference_type = std::ptrdiff_t;

            Iterator() noexcept = default;
            [[nodiscard]] HierarchyChange operator*() const noexcept;

            Iterator& operator++() noexcept
            {
                ++sequence_;
                return *this;
            }

            Iterator operator++(int) noexcept
            {
                Iterator result = *this;
                ++*this;
                return result;
            }

            [[nodiscard]] bool operator==(const Iterator&) const noexcept =
                default;

          private:
            Iterator(
                const HierarchyIndex* hierarchy,
                std::uint64_t sequence
            ) noexcept
                : hierarchy_(hierarchy), sequence_(sequence)
            {
            }

            const HierarchyIndex* hierarchy_{};
            std::uint64_t sequence_{};

            friend class HierarchyChanges;
        };

        [[nodiscard]] EChangeReadStatus status() const noexcept
        {
            return status_;
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return static_cast<std::size_t>(end_ - begin_);
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return begin_ == end_;
        }

        [[nodiscard]] Iterator begin() const noexcept
        {
            return Iterator(hierarchy_, begin_);
        }

        [[nodiscard]] Iterator end() const noexcept
        {
            return Iterator(hierarchy_, end_);
        }

      private:
        HierarchyChanges(
            const HierarchyIndex* hierarchy,
            std::uint64_t begin,
            std::uint64_t end,
            EChangeReadStatus status
        ) noexcept
            : hierarchy_(hierarchy),
              begin_(begin),
              end_(end),
              status_(status)
        {
        }

        const HierarchyIndex* hierarchy_{};
        std::uint64_t begin_{};
        std::uint64_t end_{};
        EChangeReadStatus status_{EChangeReadStatus::CURRENT};

        friend class HierarchyIndex;
    };

    class LUX_ENGINE_ECS_HIERARCHY_PUBLIC HierarchyIndex final
    {
      public:
        explicit HierarchyIndex(World& world);
        ~HierarchyIndex() noexcept;

        HierarchyIndex(const HierarchyIndex&) = delete;
        HierarchyIndex& operator=(const HierarchyIndex&) = delete;
        HierarchyIndex(HierarchyIndex&&) = delete;
        HierarchyIndex& operator=(HierarchyIndex&&) = delete;

        [[nodiscard]] bool boundTo(const World& world) const noexcept;
        [[nodiscard]] bool synchronized() const noexcept;
        [[nodiscard]] EHierarchyError lastError() const noexcept;
        [[nodiscard]] Entity parent(Entity entity) const noexcept;
        // Iteration is allocation-free and stable while the topology revision
        // does not change. Sibling order is otherwise unspecified and carries
        // no gameplay, persistence, or deterministic simulation semantics.
        [[nodiscard]] HierarchyChildren children(Entity parent) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

        [[nodiscard]] HierarchyChanges changes(
            HierarchyChangeCursor& cursor
        ) const noexcept;

      private:
        [[nodiscard]] bool canStart(const SystemStart& start) const noexcept;
        void synchronize(SystemContext& context) noexcept;
        [[nodiscard]] std::size_t visitedNodesLastUpdate() const noexcept;
        [[nodiscard]] Entity firstChild(Entity parent) const noexcept;
        [[nodiscard]] Entity nextSibling(Entity entity) const noexcept;
        [[nodiscard]] HierarchyChange changeAt(
            std::uint64_t sequence
        ) const noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend class HierarchyChildren;
        friend class HierarchyChildren::Iterator;
        friend class HierarchyChanges::Iterator;
        friend class HierarchySystem;
        friend struct detail::HierarchyIndexTestAccess;
    };

    [[nodiscard]] LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    lux::cxx::expected<void, EHierarchyError> reparent(
        WorldMutation& edit,
        Entity child,
        Entity parent
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    lux::cxx::expected<void, EHierarchyError> detach(
        WorldMutation& edit,
        Entity child
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    lux::cxx::expected<void, EHierarchyError> destroySubtree(
        WorldMutation& edit,
        Entity root
    ) noexcept;
} // namespace lux::ecs
