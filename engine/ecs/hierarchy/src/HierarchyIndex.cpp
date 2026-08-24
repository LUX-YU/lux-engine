#include <lux/engine/ecs/HierarchyIndex.hpp>

#include <lux/engine/ecs/SystemFrame.hpp>
#include <lux/engine/ecs/SystemStart.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>

#include <entt/entity/entity.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        constexpr std::size_t kHierarchyChangeCapacity = 65536U;
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
                {
                    return lux::cxx::unexpected(
                        EHierarchyError::INVALID_PARENT
                    );
                }
                const Parent* link = view.template find<Parent>(current);
                if (link == nullptr)
                    return {};
                if (link->entity == NullEntity)
                {
                    return lux::cxx::unexpected(
                        EHierarchyError::INVALID_PARENT
                    );
                }
                current = link->entity;
            }
            return lux::cxx::unexpected(EHierarchyError::CYCLE);
        }

        struct EraseOrphanedParent final
        {
            Entity child{NullEntity};
            Entity destroyed_parent{NullEntity};

            void apply(WorldEdit& edit) noexcept
            {
                const World& world = detail::WorldEditAccess::world(edit);
                const Parent* current = world.find<Parent>(child);
                if (world.valid(child) && current != nullptr &&
                    current->entity == destroyed_parent)
                {
                    edit.erase<Parent>(child);
                }
            }
        };
    } // namespace

    struct HierarchyIndex::Impl final
    {
        struct Node final
        {
            Entity identity{NullEntity};
            Entity parent{NullEntity};
            Entity first_child{NullEntity};
            Entity previous_sibling{NullEntity};
            Entity next_sibling{NullEntity};
        };

        struct RecordedChange final
        {
            std::uint64_t sequence{};
            HierarchyChange value;
        };

        explicit Impl(World& owner)
            : changes(std::make_unique<RecordedChange[]>(
                  kHierarchyChangeCapacity)),
              world(std::addressof(owner))
        {
        }

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

        [[nodiscard]] bool ensureCapacity(std::size_t index) noexcept
        {
            if (index < nodes.size())
                return true;
            try
            {
                nodes.resize(index + 1U);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        [[nodiscard]] Node* prepare(Entity entity) noexcept
        {
            const std::size_t index = entityIndex(entity);
            if (!ensureCapacity(index))
                return nullptr;
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
            detail::require(node_count != 0);
            --node_count;
        }

        [[nodiscard]] Entity detachEdge(Entity child) noexcept
        {
            Node* node = find(child);
            if (node == nullptr || node->parent == NullEntity)
                return NullEntity;

            const Entity old_parent = node->parent;
            Node* parent_node = find(old_parent);
            detail::require(parent_node != nullptr);
            if (node->previous_sibling == NullEntity)
            {
                detail::require(parent_node->first_child == child);
                parent_node->first_child = node->next_sibling;
            }
            else
            {
                Node* previous = find(node->previous_sibling);
                detail::require(previous != nullptr);
                previous->next_sibling = node->next_sibling;
            }
            if (node->next_sibling != NullEntity)
            {
                Node* next = find(node->next_sibling);
                detail::require(next != nullptr);
                next->previous_sibling = node->previous_sibling;
            }
            node->parent = NullEntity;
            node->previous_sibling = NullEntity;
            node->next_sibling = NullEntity;
            return old_parent;
        }

        void attachEdge(Entity child, Entity parent) noexcept
        {
            Node* child_node = find(child);
            Node* parent_node = find(parent);
            detail::require(
                child_node != nullptr && parent_node != nullptr &&
                child_node->parent == NullEntity
            );

            Entity previous = NullEntity;
            Entity current = parent_node->first_child;
            while (current != NullEntity && lessEntity(current, child))
            {
                previous = current;
                const Node* current_node = find(current);
                detail::require(current_node != nullptr);
                current = current_node->next_sibling;
            }

            child_node->parent = parent;
            child_node->previous_sibling = previous;
            child_node->next_sibling = current;
            if (previous == NullEntity)
                parent_node->first_child = child;
            else
                find(previous)->next_sibling = child;
            if (current != NullEntity)
                find(current)->previous_sibling = child;
        }

        void append(HierarchyChange value) noexcept
        {
            std::size_t index{};
            if (change_count == kHierarchyChangeCapacity)
            {
                change_start = (change_start + 1U) %
                    kHierarchyChangeCapacity;
                ++oldest_sequence;
                index = (change_start + change_count - 1U) %
                    kHierarchyChangeCapacity;
            }
            else
            {
                index = (change_start + change_count) %
                    kHierarchyChangeCapacity;
                ++change_count;
            }
            changes[index] = RecordedChange{next_sequence, value};
            ++next_sequence;
        }

        void establishBaseline() noexcept
        {
            ++epoch;
            if (epoch == 0)
                ++epoch;
            change_start = 0;
            change_count = 0;
            oldest_sequence = 1;
            next_sequence = 1;
        }

        void invalidate(EHierarchyError error) noexcept
        {
            synchronized = false;
            last_error = error;
            establishBaseline();
        }

        [[nodiscard]] lux::cxx::expected<void, EHierarchyError> rebuild(
            SystemFrame& frame
        ) noexcept
        {
            try
            {
                std::vector<std::pair<Entity, Entity>> edges;
                std::size_t maximum_index{};
                for (auto [child, link] : frame.query<Read<Parent>>())
                {
                    ++visited_nodes_last_update;
                    if (link.entity == NullEntity ||
                        !frame.valid(link.entity))
                    {
                        return lux::cxx::unexpected(
                            EHierarchyError::INVALID_PARENT
                        );
                    }
                    if (auto valid = validateCanonicalParent(
                            frame, child, link.entity
                        ); !valid)
                    {
                        return valid;
                    }
                    edges.emplace_back(child, link.entity);
                    maximum_index = std::max(
                        maximum_index,
                        std::max(entityIndex(child), entityIndex(link.entity))
                    );
                }

                std::vector<Node> next_nodes;
                if (!edges.empty())
                    next_nodes.resize(maximum_index + 1U);
                std::size_t next_count{};
                const auto prepare_next = [&](Entity entity) -> Node&
                {
                    Node& node = next_nodes[entityIndex(entity)];
                    if (node.identity == NullEntity)
                    {
                        node.identity = entity;
                        ++next_count;
                    }
                    detail::require(node.identity == entity);
                    return node;
                };
                for (const auto [child, parent] : edges)
                {
                    prepare_next(child).parent = parent;
                    (void)prepare_next(parent);
                }

                std::vector<std::uint8_t> colors(next_nodes.size());
                for (const Node& node : next_nodes)
                {
                    if (node.identity == NullEntity ||
                        colors[entityIndex(node.identity)] == 2U)
                    {
                        continue;
                    }
                    Entity current = node.identity;
                    while (current != NullEntity)
                    {
                        const std::size_t index = entityIndex(current);
                        if (colors[index] == 1U)
                            return lux::cxx::unexpected(EHierarchyError::CYCLE);
                        if (colors[index] == 2U)
                            break;
                        colors[index] = 1U;
                        current = next_nodes[index].parent;
                    }
                    current = node.identity;
                    while (current != NullEntity)
                    {
                        const std::size_t index = entityIndex(current);
                        if (colors[index] != 1U)
                            break;
                        colors[index] = 2U;
                        current = next_nodes[index].parent;
                    }
                }

                const auto find_next = [&next_nodes](Entity entity) -> Node*
                {
                    if (entity == NullEntity)
                        return nullptr;
                    Node& node = next_nodes[entityIndex(entity)];
                    return node.identity == entity ? std::addressof(node) : nullptr;
                };
                for (const auto [child, parent] : edges)
                {
                    Node* child_node = find_next(child);
                    Node* parent_node = find_next(parent);
                    detail::require(child_node != nullptr && parent_node != nullptr);
                    Entity previous = NullEntity;
                    Entity current = parent_node->first_child;
                    while (current != NullEntity && lessEntity(current, child))
                    {
                        previous = current;
                        current = find_next(current)->next_sibling;
                    }
                    child_node->previous_sibling = previous;
                    child_node->next_sibling = current;
                    if (previous == NullEntity)
                        parent_node->first_child = child;
                    else
                        find_next(previous)->next_sibling = child;
                    if (current != NullEntity)
                        find_next(current)->previous_sibling = child;
                }

                nodes.swap(next_nodes);
                node_count = next_count;
                synchronized = true;
                last_error = EHierarchyError::NONE;
                establishBaseline();
                return {};
            }
            catch (...)
            {
                return lux::cxx::unexpected(
                    EHierarchyError::ALLOCATION_FAILURE
                );
            }
        }

        void recordDetached(Entity child, Entity old_parent) noexcept
        {
            append(HierarchyChange{
                child,
                old_parent,
                NullEntity,
                EHierarchyChangeKind::DETACHED});
        }

        void removeEntity(
            Entity entity,
            WorldCommands commands
        ) noexcept
        {
            Node* node = find(entity);
            if (node == nullptr)
                return;

            const Entity old_parent = detachEdge(entity);
            if (old_parent != NullEntity)
                recordDetached(entity, old_parent);

            node = find(entity);
            while (node != nullptr && node->first_child != NullEntity)
            {
                const Entity child = node->first_child;
                const Entity detached_from = detachEdge(child);
                detail::require(detached_from == entity);
                recordDetached(child, entity);
                if (commands.push(EraseOrphanedParent{child, entity}) !=
                    ECommandResult::ACCEPTED)
                {
                    last_error = EHierarchyError::ALLOCATION_FAILURE;
                    synchronized = false;
                }
                prune(child);
                node = find(entity);
            }

            node = find(entity);
            if (node != nullptr)
            {
                *node = {};
                detail::require(node_count != 0);
                --node_count;
            }
            prune(old_parent);
        }

        void applyParent(Entity child, const Parent* link) noexcept
        {
            Node* child_node = find(child);
            const Entity previous = child_node == nullptr
                ? NullEntity
                : child_node->parent;
            if (link == nullptr)
            {
                if (previous == NullEntity)
                    return;
                const Entity old_parent = detachEdge(child);
                recordDetached(child, old_parent);
                prune(child);
                prune(old_parent);
                return;
            }
            if (previous == link->entity)
                return;

            child_node = prepare(child);
            Node* parent_node = prepare(link->entity);
            if (child_node == nullptr || parent_node == nullptr)
            {
                invalidate(EHierarchyError::INVALID_ENTITY);
                return;
            }
            const Entity old_parent = detachEdge(child);
            attachEdge(child, link->entity);
            append(HierarchyChange{
                child,
                old_parent,
                link->entity,
                old_parent == NullEntity
                    ? EHierarchyChangeKind::ATTACHED
                    : EHierarchyChangeKind::REPARENTED});
            prune(old_parent);
        }

        void synchronize(SystemFrame& frame) noexcept
        {
            visited_nodes_last_update = 0U;
            auto parent_changes = frame.changes(parent_cursor);
            auto entity_changes = frame.entityChanges(entity_cursor);
            if (parent_changes.status() == EChangeReadStatus::RESYNC_REQUIRED ||
                entity_changes.status() == EChangeReadStatus::RESYNC_REQUIRED ||
                !synchronized)
            {
                auto rebuilt = rebuild(frame);
                if (!rebuilt)
                    invalidate(rebuilt.error());
                return;
            }

            std::size_t maximum_index = nodes.empty() ? 0U : nodes.size() - 1U;
            for (const ComponentChange change : parent_changes)
            {
                ++visited_nodes_last_update;
                maximum_index = std::max(
                    maximum_index, entityIndex(change.entity)
                );
                const Parent* link = frame.find<Parent>(change.entity);
                if (link == nullptr)
                    continue;
                maximum_index = std::max(
                    maximum_index, entityIndex(link->entity)
                );
                if (auto valid = validateCanonicalParent(
                        frame, change.entity, link->entity
                    ); !valid)
                {
                    invalidate(valid.error());
                    return;
                }
            }
            if (!ensureCapacity(maximum_index))
            {
                invalidate(EHierarchyError::ALLOCATION_FAILURE);
                return;
            }

            const WorldCommands commands = frame.commands();
            for (const EntityChange change : entity_changes)
            {
                ++visited_nodes_last_update;
                if (change.kind == EEntityChangeKind::DESTROYED)
                    removeEntity(change.entity, commands);
            }
            if (!synchronized)
                return;
            for (const ComponentChange change : parent_changes)
            {
                if (!frame.valid(change.entity))
                    continue;
                applyParent(change.entity, frame.find<Parent>(change.entity));
                if (!synchronized)
                    return;
            }
            last_error = EHierarchyError::NONE;
        }

        std::vector<Node> nodes;
        std::unique_ptr<RecordedChange[]> changes;
        World* world{};
        std::size_t node_count{};
        std::size_t change_start{};
        std::size_t change_count{};
        std::uint64_t oldest_sequence{1};
        std::uint64_t next_sequence{1};
        std::uint32_t epoch{1};
        ChangeCursor<Parent> parent_cursor;
        EntityChangeCursor entity_cursor;
        std::size_t visited_nodes_last_update{};
        EHierarchyError last_error{EHierarchyError::NONE};
        bool synchronized{};
    };

    HierarchyIndex::HierarchyIndex(World& world)
        : impl_(std::make_unique<Impl>(world))
    {
    }

    HierarchyIndex::~HierarchyIndex() noexcept = default;

    bool HierarchyIndex::boundTo(const World& world) const noexcept
    {
        return impl_->world == std::addressof(world);
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

    HierarchyChanges HierarchyIndex::changes(
        HierarchyChangeCursor& cursor
    ) const noexcept
    {
        if (!impl_->synchronized || cursor.epoch_ != impl_->epoch ||
            cursor.sequence_ == 0 ||
            cursor.sequence_ < impl_->oldest_sequence ||
            cursor.sequence_ > impl_->next_sequence)
        {
            cursor.epoch_ = impl_->epoch;
            cursor.sequence_ = impl_->next_sequence;
            return HierarchyChanges(
                this,
                impl_->next_sequence,
                impl_->next_sequence,
                EChangeReadStatus::RESYNC_REQUIRED
            );
        }
        const std::uint64_t begin = cursor.sequence_;
        cursor.sequence_ = impl_->next_sequence;
        return HierarchyChanges(
            this,
            begin,
            impl_->next_sequence,
            EChangeReadStatus::CURRENT
        );
    }

    bool HierarchyIndex::canStart(const SystemStart& start) const noexcept
    {
        return start.boundTo(*impl_->world);
    }

    void HierarchyIndex::synchronize(SystemFrame& frame) noexcept
    {
        impl_->synchronize(frame);
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

    HierarchyChange HierarchyIndex::changeAt(
        std::uint64_t sequence
    ) const noexcept
    {
        detail::require(
            sequence >= impl_->oldest_sequence &&
            sequence < impl_->next_sequence
        );
        const std::size_t offset = static_cast<std::size_t>(
            sequence - impl_->oldest_sequence
        );
        detail::require(offset < impl_->change_count);
        const std::size_t index = (impl_->change_start + offset) %
            kHierarchyChangeCapacity;
        const Impl::RecordedChange& recorded = impl_->changes[index];
        detail::require(recorded.sequence == sequence);
        return recorded.value;
    }

    HierarchyChildren::Iterator& HierarchyChildren::Iterator::operator++()
        noexcept
    {
        detail::require(hierarchy_ != nullptr && entity_ != NullEntity);
        entity_ = hierarchy_->nextSibling(entity_);
        return *this;
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

    HierarchyChange HierarchyChanges::Iterator::operator*() const noexcept
    {
        detail::require(hierarchy_ != nullptr);
        return hierarchy_->changeAt(sequence_);
    }

    lux::cxx::expected<void, EHierarchyError> reparent(
        WorldEdit& edit,
        Entity child,
        Entity parent
    ) noexcept
    {
        World& world = detail::WorldEditAccess::world(edit);
        if (auto valid = validateCanonicalParent(world, child, parent); !valid)
            return valid;

        const Parent* current = world.find<Parent>(child);
        if (current != nullptr && current->entity == parent)
            return {};
        try
        {
            if (current == nullptr)
                edit.emplace<Parent>(child, parent);
            else
            {
                edit.update<Parent>(child, [parent](Parent& value) noexcept
                {
                    value.entity = parent;
                });
            }
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(
                EHierarchyError::ALLOCATION_FAILURE
            );
        }
    }

    lux::cxx::expected<void, EHierarchyError> detach(
        WorldEdit& edit,
        Entity child
    ) noexcept
    {
        World& world = detail::WorldEditAccess::world(edit);
        if (!world.valid(child))
            return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);
        if (world.find<Parent>(child) != nullptr)
            edit.erase<Parent>(child);
        return {};
    }

    lux::cxx::expected<void, EHierarchyError> destroySubtree(
        WorldEdit& edit,
        Entity root
    ) noexcept
    {
        World& world = detail::WorldEditAccess::world(edit);
        if (!world.valid(root))
            return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);

        try
        {
            std::vector<std::pair<Entity, Entity>> edges;
            for (auto [child, parent] : edit.query<Read<Parent>>())
            {
                if (auto valid = validateCanonicalParent(
                        world, child, parent.entity
                    ); !valid)
                {
                    return valid;
                }
                edges.emplace_back(parent.entity, child);
            }
            std::sort(
                edges.begin(), edges.end(),
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
                    edges.begin(), edges.end(), parent,
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
                detail::require(world.valid(*iterator));
                edit.destroy(*iterator);
            }
            return {};
        }
        catch (...)
        {
            return lux::cxx::unexpected(
                EHierarchyError::ALLOCATION_FAILURE
            );
        }
    }
} // namespace lux::ecs
