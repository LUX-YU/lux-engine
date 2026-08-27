#pragma once

#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/ecs/hierarchy/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <vector>

namespace lux::simulation::ecs
{
    namespace detail
    {
        struct HierarchyMutation;
        class HierarchyMaintenance;
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
        CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
    };

    enum class EHierarchyDeltaKind : std::uint8_t
    {
        ATTACHED,
        REPARENTED,
        DETACHED,
    };

    struct HierarchyDelta final
    {
        Entity entity{NullEntity};
        Entity previous_parent{NullEntity};
        Entity parent{NullEntity};
        EHierarchyDeltaKind kind{EHierarchyDeltaKind::DETACHED};
    };

    class LUX_ENGINE_SIMULATION_ECS_HIERARCHY_PUBLIC HierarchyDeltaBatch final
    {
    public:
        [[nodiscard]] lux::cxx::expected<void, EHierarchyError> prepare(std::size_t capacity) noexcept;
        void reset() noexcept;
        [[nodiscard]] bool exact() const noexcept;
        [[nodiscard]] std::span<const HierarchyDelta> values() const noexcept;
        [[nodiscard]] std::size_t capacity() const noexcept;

    private:
        [[nodiscard]] bool append(HierarchyDelta delta) noexcept;
        void requireRebuild() noexcept;

        std::vector<HierarchyDelta> values_;
        std::size_t capacity_{};
        bool exact_{true};

        friend class HierarchyIndex;
        friend class detail::HierarchyMaintenance;
    };

    class HierarchyIndex;

    class LUX_ENGINE_SIMULATION_ECS_HIERARCHY_PUBLIC HierarchyChildren final
    {
    public:
        class LUX_ENGINE_SIMULATION_ECS_HIERARCHY_PUBLIC Iterator final
        {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = Entity;
            using difference_type = std::ptrdiff_t;

            Iterator() noexcept = default;
            [[nodiscard]] Entity operator*() const noexcept;
            Iterator& operator++() noexcept;
            Iterator operator++(int) noexcept;
            [[nodiscard]] bool operator==(const Iterator&) const noexcept = default;

        private:
            Iterator(const HierarchyIndex* hierarchy, Entity entity) noexcept;
            const HierarchyIndex* hierarchy_{};
            Entity entity_{NullEntity};
            friend class HierarchyChildren;
        };

        [[nodiscard]] Iterator begin() const noexcept;
        [[nodiscard]] Iterator end() const noexcept;
        [[nodiscard]] bool empty() const noexcept;

    private:
        HierarchyChildren(const HierarchyIndex& hierarchy, Entity parent) noexcept;
        const HierarchyIndex* hierarchy_{};
        Entity parent_{NullEntity};
        friend class HierarchyIndex;
    };

    class LUX_ENGINE_SIMULATION_ECS_HIERARCHY_PUBLIC HierarchyIndex final
    {
    public:
        HierarchyIndex();
        ~HierarchyIndex() noexcept;
        HierarchyIndex(const HierarchyIndex&) = delete;
        HierarchyIndex& operator=(const HierarchyIndex&) = delete;
        HierarchyIndex(HierarchyIndex&&) = delete;
        HierarchyIndex& operator=(HierarchyIndex&&) = delete;

        [[nodiscard]] bool synchronized() const noexcept;
        [[nodiscard]] EHierarchyError lastError() const noexcept;
        [[nodiscard]] Entity parent(Entity entity) const noexcept;
        [[nodiscard]] HierarchyChildren children(Entity parent) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

    private:
        [[nodiscard]] lux::cxx::expected<void, EHierarchyError> prepare(std::size_t relation_capacity) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EHierarchyError>
        apply(std::span<const detail::HierarchyMutation> mutations, HierarchyDeltaBatch& deltas) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EHierarchyError>
        rebuild(std::span<const detail::HierarchyMutation> canonical_relations, HierarchyDeltaBatch& deltas) noexcept;
        void invalidate(EHierarchyError error) noexcept;
        [[nodiscard]] std::size_t visitedNodesLastUpdate() const noexcept;
        [[nodiscard]] Entity firstChild(Entity parent) const noexcept;
        [[nodiscard]] Entity nextSibling(Entity entity) const noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend class HierarchyChildren;
        friend class HierarchyChildren::Iterator;
        friend class detail::HierarchyMaintenance;
        friend struct detail::HierarchyIndexTestAccess;
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_HIERARCHY_PUBLIC lux::cxx::expected<void, EHierarchyError>
    reparent(Registry& registry, Entity child, Entity parent) noexcept;

    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_HIERARCHY_PUBLIC lux::cxx::expected<void, EHierarchyError>
    detach(Registry& registry, Entity child) noexcept;
}
