#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>

#include <lux/engine/simulation/ecs/EcsState.hpp>
#include <lux/engine/simulation/ecs/SimulationEcsMutation.hpp>
#include <lux/engine/simulation/ecs/core/detail/EcsStateAccess.hpp>

#include <entt/entity/entity.hpp>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace lux::simulation::ecs
{
    namespace
    {
        constexpr std::size_t kMaximumParentDepth =
            static_cast<std::size_t>(
                entt::entt_traits<Entity>::entity_mask
            ) + 1U;

        [[nodiscard]] std::size_t entityIndex(Entity entity) noexcept
        {
            return static_cast<std::size_t>(entt::to_entity(entity));
        }

        [[nodiscard]] bool lessEntity(Entity left, Entity right) noexcept
        {
            return entityBits(left) < entityBits(right);
        }

        template <class View>
        [[nodiscard]] lux::cxx::expected<void, EHierarchyError>
        validateCanonicalParent(
            const View& view,
            Entity child,
            Entity parent
        ) noexcept
        {
            if (!view.valid(child) || !view.valid(parent))
                return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);
            if (child == parent)
                return lux::cxx::unexpected(EHierarchyError::SELF_PARENT);

            Entity current = parent;
            for (std::size_t depth{}; depth < kMaximumParentDepth; ++depth)
            {
                if (current == child)
                    return lux::cxx::unexpected(EHierarchyError::CYCLE);
                if (!view.valid(current))
                    return lux::cxx::unexpected(EHierarchyError::INVALID_PARENT);
                const Parent* link = view.template find<Parent>(current);
                if (link == nullptr)
                    return {};
                if (link->entity == NullEntity)
                    return lux::cxx::unexpected(EHierarchyError::INVALID_PARENT);
                current = link->entity;
            }
            return lux::cxx::unexpected(EHierarchyError::CYCLE);
        }
    } // namespace

    lux::cxx::expected<void, EHierarchyError>
    HierarchyMutationBatch::prepare(std::size_t capacity) noexcept
    {
        try
        {
            std::vector<HierarchyMutation> replacement;
            replacement.reserve(capacity);
            values_.swap(replacement);
            capacity_ = capacity;
            exact_ = true;
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(EHierarchyError::ALLOCATION_FAILURE);
        }
    }

    void HierarchyMutationBatch::reset() noexcept
    {
        values_.clear();
        exact_ = true;
    }

    bool HierarchyMutationBatch::append(HierarchyMutation mutation) noexcept
    {
        if (!exact_)
            return false;
        if (values_.size() >= capacity_)
        {
            exact_ = false;
            return false;
        }
        values_.push_back(mutation);
        return true;
    }

    bool HierarchyMutationBatch::exact() const noexcept
    {
        return exact_;
    }

    std::span<const HierarchyMutation>
    HierarchyMutationBatch::values() const noexcept
    {
        return values_;
    }

    std::size_t HierarchyMutationBatch::capacity() const noexcept
    {
        return capacity_;
    }

    lux::cxx::expected<void, EHierarchyError>
    HierarchyDeltaBatch::prepare(std::size_t capacity) noexcept
    {
        try
        {
            std::vector<HierarchyDelta> replacement;
            replacement.reserve(capacity);
            values_.swap(replacement);
            capacity_ = capacity;
            exact_ = true;
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(EHierarchyError::ALLOCATION_FAILURE);
        }
    }

    void HierarchyDeltaBatch::reset() noexcept
    {
        values_.clear();
        exact_ = true;
    }

    bool HierarchyDeltaBatch::append(HierarchyDelta delta) noexcept
    {
        if (!exact_)
            return false;
        if (values_.size() >= capacity_)
        {
            requireRebuild();
            return false;
        }
        values_.push_back(delta);
        return true;
    }

    void HierarchyDeltaBatch::requireRebuild() noexcept
    {
        values_.clear();
        exact_ = false;
    }

    bool HierarchyDeltaBatch::exact() const noexcept
    {
        return exact_;
    }

    std::span<const HierarchyDelta> HierarchyDeltaBatch::values() const noexcept
    {
        return values_;
    }

    std::size_t HierarchyDeltaBatch::capacity() const noexcept
    {
        return capacity_;
    }

    struct HierarchyIndex::Impl final
    {
        struct Node final
        {
            Entity identity{NullEntity};
            Entity parent{NullEntity};
            Entity first_child{NullEntity};
            Entity last_child{NullEntity};
            Entity previous_sibling{NullEntity};
            Entity next_sibling{NullEntity};
        };

        [[nodiscard]] Node* find(Entity entity) noexcept
        {
            if (entity == NullEntity)
                return nullptr;
            const std::size_t index = entityIndex(entity);
            if (index >= nodes.size() || nodes[index].identity != entity)
                return nullptr;
            return std::addressof(nodes[index]);
        }

        [[nodiscard]] const Node* find(Entity entity) const noexcept
        {
            return const_cast<Impl*>(this)->find(entity);
        }

        [[nodiscard]] Node* prepare(Entity entity)
        {
            if (entity == NullEntity)
                return nullptr;
            const std::size_t index = entityIndex(entity);
            if (index >= nodes.size())
                nodes.resize(index + 1U);
            Node& node = nodes[index];
            if (node.identity == NullEntity)
            {
                node.identity = entity;
                ++node_count;
            }
            else if (node.identity != entity)
                return nullptr;
            return std::addressof(node);
        }

        void prune(Entity entity) noexcept
        {
            Node* node = find(entity);
            if (node == nullptr || node->parent != NullEntity ||
                node->first_child != NullEntity)
            {
                return;
            }
            *node = {};
            detail::require(node_count != 0U);
            --node_count;
        }

        [[nodiscard]] Entity detachEdge(Entity child) noexcept
        {
            Node* node = find(child);
            if (node == nullptr || node->parent == NullEntity)
                return NullEntity;
            const Entity previous_parent = node->parent;
            Node* parent = find(previous_parent);
            detail::require(parent != nullptr);
            if (node->previous_sibling == NullEntity)
                parent->first_child = node->next_sibling;
            else
                find(node->previous_sibling)->next_sibling = node->next_sibling;
            if (node->next_sibling == NullEntity)
                parent->last_child = node->previous_sibling;
            else
                find(node->next_sibling)->previous_sibling =
                    node->previous_sibling;
            if (parent->first_child == NullEntity)
                parent->last_child = NullEntity;
            node->parent = NullEntity;
            node->previous_sibling = NullEntity;
            node->next_sibling = NullEntity;
            return previous_parent;
        }

        void attachEdge(Entity child, Entity parent) noexcept
        {
            Node* child_node = find(child);
            Node* parent_node = find(parent);
            detail::require(
                child_node != nullptr && parent_node != nullptr &&
                child_node->parent == NullEntity
            );
            const Entity previous = parent_node->last_child;
            child_node->parent = parent;
            child_node->previous_sibling = previous;
            if (previous == NullEntity)
                parent_node->first_child = child;
            else
                find(previous)->next_sibling = child;
            parent_node->last_child = child;
        }

        [[nodiscard]] bool wouldCycle(Entity child, Entity parent) const noexcept
        {
            Entity current = parent;
            for (std::size_t depth{}; depth < kMaximumParentDepth; ++depth)
            {
                if (current == child)
                    return true;
                const Node* node = find(current);
                if (node == nullptr || node->parent == NullEntity)
                    return false;
                current = node->parent;
            }
            return true;
        }

        [[nodiscard]] lux::cxx::expected<void, EHierarchyError> applyParent(
            Entity child,
            Entity parent,
            HierarchyDeltaBatch* deltas
        )
        {
            if (child == NullEntity || parent == NullEntity)
                return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);
            if (child == parent)
                return lux::cxx::unexpected(EHierarchyError::SELF_PARENT);
            if (wouldCycle(child, parent))
                return lux::cxx::unexpected(EHierarchyError::CYCLE);

            Node* child_node = find(child);
            const Entity previous = child_node == nullptr
                ? NullEntity
                : child_node->parent;
            if (previous == parent)
                return {};
            if (prepare(child) == nullptr || prepare(parent) == nullptr)
                return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);

            (void)detachEdge(child);
            attachEdge(child, parent);
            if (deltas != nullptr)
            {
                (void)deltas->append(HierarchyDelta{
                    child,
                    previous,
                    parent,
                    previous == NullEntity
                        ? EHierarchyDeltaKind::ATTACHED
                        : EHierarchyDeltaKind::REPARENTED
                });
            }
            prune(previous);
            return {};
        }

        void removeParent(Entity child, HierarchyDeltaBatch* deltas) noexcept
        {
            const Entity previous = detachEdge(child);
            if (previous == NullEntity)
                return;
            if (deltas != nullptr)
            {
                (void)deltas->append(HierarchyDelta{
                    child,
                    previous,
                    NullEntity,
                    EHierarchyDeltaKind::DETACHED
                });
            }
            prune(child);
            prune(previous);
        }

        void removeEntity(Entity entity, HierarchyDeltaBatch* deltas) noexcept
        {
            Node* node = find(entity);
            if (node == nullptr)
                return;
            removeParent(entity, deltas);
            node = find(entity);
            while (node != nullptr && node->first_child != NullEntity)
            {
                const Entity child = node->first_child;
                removeParent(child, deltas);
                node = find(entity);
            }
            node = find(entity);
            if (node != nullptr)
            {
                *node = {};
                detail::require(node_count != 0U);
                --node_count;
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EHierarchyError> applyOne(
            HierarchyMutation mutation,
            HierarchyDeltaBatch* deltas
        )
        {
            switch (mutation.kind)
            {
            case EHierarchyMutationKind::SET_PARENT:
                return applyParent(mutation.entity, mutation.parent, deltas);
            case EHierarchyMutationKind::REMOVE_PARENT:
                removeParent(mutation.entity, deltas);
                return {};
            case EHierarchyMutationKind::ENTITY_DESTROYED:
                removeEntity(mutation.entity, deltas);
                return {};
            }
            return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);
        }

        std::vector<Node> nodes;
        std::size_t node_count{};
        std::size_t visited_nodes_last_update{};
        EHierarchyError last_error{EHierarchyError::NONE};
        bool synchronized{};
    };

    HierarchyIndex::HierarchyIndex()
        : impl_(std::make_unique<Impl>())
    {
    }

    HierarchyIndex::~HierarchyIndex() noexcept = default;

    lux::cxx::expected<void, EHierarchyError> HierarchyIndex::apply(
        std::span<const HierarchyMutation> mutations,
        HierarchyDeltaBatch& deltas
    ) noexcept
    {
        impl_->visited_nodes_last_update = 0U;
        if (!impl_->synchronized)
            return lux::cxx::unexpected(EHierarchyError::NOT_SYNCHRONIZED);
        try
        {
            for (const HierarchyMutation mutation : mutations)
            {
                ++impl_->visited_nodes_last_update;
                auto applied = impl_->applyOne(mutation, &deltas);
                if (!applied)
                {
                    invalidate(applied.error());
                    return applied;
                }
            }
            impl_->last_error = EHierarchyError::NONE;
            return {};
        }
        catch (...)
        {
            invalidate(EHierarchyError::ALLOCATION_FAILURE);
            return lux::cxx::unexpected(EHierarchyError::ALLOCATION_FAILURE);
        }
    }

    lux::cxx::expected<void, EHierarchyError> HierarchyIndex::rebuild(
        std::span<const HierarchyMutation> canonical_relations,
        HierarchyDeltaBatch& deltas
    ) noexcept
    {
        try
        {
            auto replacement = std::make_unique<Impl>();
            replacement->synchronized = true;
            for (const HierarchyMutation relation : canonical_relations)
            {
                if (relation.kind != EHierarchyMutationKind::SET_PARENT)
                {
                    invalidate(EHierarchyError::INVALID_ENTITY);
                    return lux::cxx::unexpected(
                        EHierarchyError::INVALID_ENTITY
                    );
                }
                ++replacement->visited_nodes_last_update;
                auto applied = replacement->applyOne(relation, nullptr);
                if (!applied)
                {
                    invalidate(applied.error());
                    return applied;
                }
            }
            replacement->last_error = EHierarchyError::NONE;
            impl_.swap(replacement);
            deltas.requireRebuild();
            return {};
        }
        catch (...)
        {
            invalidate(EHierarchyError::ALLOCATION_FAILURE);
            return lux::cxx::unexpected(EHierarchyError::ALLOCATION_FAILURE);
        }
    }

    void HierarchyIndex::invalidate(EHierarchyError error) noexcept
    {
        impl_->synchronized = false;
        impl_->last_error = error;
    }

    bool HierarchyIndex::synchronized() const noexcept
    {
        return impl_->synchronized;
    }

    EHierarchyError HierarchyIndex::lastError() const noexcept
    {
        return impl_->last_error;
    }

    Entity HierarchyIndex::parent(Entity entity) const noexcept
    {
        const Impl::Node* node = impl_->find(entity);
        return node == nullptr ? NullEntity : node->parent;
    }

    HierarchyChildren HierarchyIndex::children(Entity parent) const noexcept
    {
        return HierarchyChildren(*this, parent);
    }

    std::size_t HierarchyIndex::size() const noexcept
    {
        return impl_->node_count;
    }

    std::size_t HierarchyIndex::visitedNodesLastUpdate() const noexcept
    {
        return impl_->visited_nodes_last_update;
    }

    Entity HierarchyIndex::firstChild(Entity parent) const noexcept
    {
        const Impl::Node* node = impl_->find(parent);
        return node == nullptr ? NullEntity : node->first_child;
    }

    Entity HierarchyIndex::nextSibling(Entity entity) const noexcept
    {
        const Impl::Node* node = impl_->find(entity);
        return node == nullptr ? NullEntity : node->next_sibling;
    }

    HierarchyChildren::Iterator::Iterator(
        const HierarchyIndex* hierarchy,
        Entity entity
    ) noexcept
        : hierarchy_(hierarchy), entity_(entity)
    {
    }

    Entity HierarchyChildren::Iterator::operator*() const noexcept
    {
        return entity_;
    }

    HierarchyChildren::Iterator& HierarchyChildren::Iterator::operator++()
        noexcept
    {
        detail::require(hierarchy_ != nullptr && entity_ != NullEntity);
        entity_ = hierarchy_->nextSibling(entity_);
        return *this;
    }

    HierarchyChildren::Iterator HierarchyChildren::Iterator::operator++(
        int
    ) noexcept
    {
        Iterator result = *this;
        ++*this;
        return result;
    }

    HierarchyChildren::HierarchyChildren(
        const HierarchyIndex& hierarchy,
        Entity parent
    ) noexcept
        : hierarchy_(std::addressof(hierarchy)), parent_(parent)
    {
    }

    HierarchyChildren::Iterator HierarchyChildren::begin() const noexcept
    {
        return Iterator(hierarchy_, hierarchy_->firstChild(parent_));
    }

    HierarchyChildren::Iterator HierarchyChildren::end() const noexcept
    {
        return Iterator(hierarchy_, NullEntity);
    }

    bool HierarchyChildren::empty() const noexcept
    {
        return hierarchy_->firstChild(parent_) == NullEntity;
    }

    lux::cxx::expected<void, EHierarchyError> reparent(
        EcsMutation& mutation,
        Entity child,
        Entity parent
    ) noexcept
    {
        EcsState& state = detail::EcsMutationAccess::world(mutation);
        if (auto valid = validateCanonicalParent(state, child, parent); !valid)
            return valid;
        const Parent* current = state.find<Parent>(child);
        if (current != nullptr && current->entity == parent)
            return {};
        try
        {
            if (current == nullptr)
                mutation.emplace<Parent>(child, parent);
            else
            {
                mutation.update<Parent>(child, [parent](Parent& value) noexcept
                {
                    value.entity = parent;
                });
            }
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(EHierarchyError::ALLOCATION_FAILURE);
        }
    }

    lux::cxx::expected<void, EHierarchyError> reparent(
        SimulationEcsMutation& mutation,
        Entity child,
        Entity parent
    ) noexcept
    {
        const EcsState& state = mutation.state();
        if (auto valid = validateCanonicalParent(state, child, parent); !valid)
            return valid;
        const Parent* current = state.find<Parent>(child);
        if (current != nullptr && current->entity == parent)
            return {};
        try
        {
            if (current == nullptr)
                mutation.emplace<Parent>(child, parent);
            else
            {
                mutation.update<Parent>(
                    child,
                    [parent](Parent& value) noexcept
                    {
                        value.entity = parent;
                    }
                );
            }
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(EHierarchyError::ALLOCATION_FAILURE);
        }
    }

    lux::cxx::expected<void, EHierarchyError> detach(
        EcsMutation& mutation,
        Entity child
    ) noexcept
    {
        EcsState& state = detail::EcsMutationAccess::world(mutation);
        if (!state.valid(child))
            return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);
        if (state.find<Parent>(child) != nullptr)
            mutation.erase<Parent>(child);
        return {};
    }

    lux::cxx::expected<void, EHierarchyError> destroySubtree(
        EcsMutation& mutation,
        Entity root
    ) noexcept
    {
        EcsState& state = detail::EcsMutationAccess::world(mutation);
        if (!state.valid(root))
            return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);

        try
        {
            std::vector<std::pair<Entity, Entity>> edges;
            std::size_t maximum_index{};
            for (auto [child, parent] : mutation.query<Read<Parent>>())
            {
                if (parent.entity == NullEntity || !state.valid(parent.entity))
                    return lux::cxx::unexpected(EHierarchyError::INVALID_PARENT);
                if (child == parent.entity)
                    return lux::cxx::unexpected(EHierarchyError::SELF_PARENT);
                edges.emplace_back(parent.entity, child);
                maximum_index = std::max(
                    maximum_index,
                    std::max(entityIndex(child), entityIndex(parent.entity))
                );
            }

            std::vector<Entity> parents;
            std::vector<std::uint8_t> colors;
            if (!edges.empty())
            {
                parents.resize(maximum_index + 1U, NullEntity);
                colors.resize(maximum_index + 1U);
            }
            for (const auto [parent, child] : edges)
                parents[entityIndex(child)] = parent;
            for (const auto [unused_parent, child] : edges)
            {
                (void)unused_parent;
                if (colors[entityIndex(child)] == 2U)
                    continue;
                Entity current = child;
                while (current != NullEntity)
                {
                    const std::size_t index = entityIndex(current);
                    if (colors[index] == 1U)
                        return lux::cxx::unexpected(EHierarchyError::CYCLE);
                    if (colors[index] == 2U)
                        break;
                    colors[index] = 1U;
                    current = parents[index];
                }
                current = child;
                while (current != NullEntity)
                {
                    const std::size_t index = entityIndex(current);
                    if (colors[index] != 1U)
                        break;
                    colors[index] = 2U;
                    current = parents[index];
                }
            }
            std::sort(
                edges.begin(),
                edges.end(),
                [](const auto& left, const auto& right)
                {
                    if (left.first != right.first)
                        return lessEntity(left.first, right.first);
                    return lessEntity(left.second, right.second);
                }
            );

            std::vector<Entity> order;
            std::vector<Entity> stack;
            order.reserve(edges.size() + 1U);
            stack.reserve(edges.size() + 1U);
            order.push_back(root);
            stack.push_back(root);
            while (!stack.empty())
            {
                const Entity parent = stack.back();
                stack.pop_back();
                const auto begin = std::lower_bound(
                    edges.begin(),
                    edges.end(),
                    parent,
                    [](const auto& edge, Entity value)
                    {
                        return lessEntity(edge.first, value);
                    }
                );
                for (auto iterator = begin;
                     iterator != edges.end() && iterator->first == parent;
                     ++iterator)
                {
                    order.push_back(iterator->second);
                    stack.push_back(iterator->second);
                }
            }
            for (auto iterator = order.rbegin(); iterator != order.rend(); ++iterator)
            {
                detail::require(state.valid(*iterator));
                mutation.destroy(*iterator);
            }
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(EHierarchyError::ALLOCATION_FAILURE);
        }
    }
} // namespace lux::simulation::ecs
