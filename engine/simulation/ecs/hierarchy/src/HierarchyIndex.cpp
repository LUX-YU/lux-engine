#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>

#include <lux/engine/simulation/ecs/EcsTaskResources.hpp>
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

            void apply(SimulationEcsMutation& edit) noexcept
            {
                const EcsState& world = edit.state();
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
            Entity last_child{NullEntity};
            Entity previous_sibling{NullEntity};
            Entity next_sibling{NullEntity};
            Entity repair_parent{NullEntity};
            Entity repair_previous{NullEntity};
            Entity repair_next{NullEntity};
            bool repair_command_queued{};
        };

        struct RecordedChange final
        {
            std::uint64_t sequence{};
            HierarchyChange value;
        };

        explicit Impl(EcsState& owner)
            : world(std::addressof(owner))
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
                node->first_child != NullEntity ||
                node->repair_parent != NullEntity)
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
            else
            {
                detail::require(parent_node->last_child == child);
                parent_node->last_child = node->previous_sibling;
            }
            if (parent_node->first_child == NullEntity)
                parent_node->last_child = NullEntity;
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

            const Entity previous = parent_node->last_child;
            child_node->parent = parent;
            child_node->previous_sibling = previous;
            child_node->next_sibling = NullEntity;
            if (previous == NullEntity)
                parent_node->first_child = child;
            else
                find(previous)->next_sibling = child;
            parent_node->last_child = child;
        }

        void append(HierarchyChange value) noexcept
        {
            if (change_count == kHierarchyChangeCapacity)
                establishBaseline();

            if (change_count == changes.size())
            {
                try
                {
                    const std::size_t next_capacity = std::min(
                        kHierarchyChangeCapacity,
                        std::max<std::size_t>(256U, changes.size() * 2U)
                    );
                    std::vector<RecordedChange> replacement(next_capacity);
                    for (std::size_t offset{}; offset < change_count; ++offset)
                    {
                        replacement[offset] = changes[
                            (change_start + offset) % changes.size()
                        ];
                    }
                    changes.swap(replacement);
                    change_start = 0U;
                }
                catch (...)
                {
                    establishBaseline();
                    return;
                }
            }

            const std::size_t index = (change_start + change_count) %
                changes.size();
            changes[index] = RecordedChange{next_sequence, value};
            ++change_count;
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

        void unlinkRepair(Node& node) noexcept
        {
            detail::require(node.repair_parent != NullEntity);
            if (node.repair_previous == NullEntity)
                repair_head = node.repair_next;
            else
                find(node.repair_previous)->repair_next = node.repair_next;
            if (node.repair_next == NullEntity)
                repair_tail = node.repair_previous;
            else
                find(node.repair_next)->repair_previous =
                    node.repair_previous;
            node.repair_parent = NullEntity;
            node.repair_previous = NullEntity;
            node.repair_next = NullEntity;
            node.repair_command_queued = false;
        }

        [[nodiscard]] bool queueRepair(
            Entity child,
            Entity expected_parent,
            bool command_queued
        ) noexcept
        {
            Node* node = prepare(child);
            if (node == nullptr)
                return false;
            if (node->repair_parent != NullEntity)
            {
                node->repair_parent = expected_parent;
                node->repair_command_queued = command_queued;
                return true;
            }
            node->repair_parent = expected_parent;
            node->repair_previous = repair_tail;
            node->repair_next = NullEntity;
            node->repair_command_queued = command_queued;
            if (repair_tail == NullEntity)
                repair_head = child;
            else
                find(repair_tail)->repair_next = child;
            repair_tail = child;
            return true;
        }

        void cancelRepair(Entity child) noexcept
        {
            Node* node = find(child);
            if (node != nullptr && node->repair_parent != NullEntity)
                unlinkRepair(*node);
        }

        [[nodiscard]] bool retryRepairs(EcsCommands commands) noexcept
        {
            Entity current = repair_head;
            while (current != NullEntity)
            {
                Node* node = find(current);
                detail::require(
                    node != nullptr && node->repair_parent != NullEntity
                );
                const Entity next = node->repair_next;
                const Entity expected_parent = node->repair_parent;
                const Parent* canonical = world->find<Parent>(current);
                if (!world->valid(current) || canonical == nullptr ||
                    canonical->entity != expected_parent)
                {
                    unlinkRepair(*node);
                    prune(current);
                    current = next;
                    continue;
                }

                // A command accepted during the previous update must have run
                // at that phase's barrier. If canonical data still matches,
                // retry rather than assuming publication succeeded.
                node->repair_command_queued = false;
                const ECommandResult result = commands.push(
                    EraseOrphanedParent{current, expected_parent}
                );
                node->repair_command_queued =
                    result == ECommandResult::ACCEPTED;
                if (result != ECommandResult::ACCEPTED)
                    last_error = EHierarchyError::ALLOCATION_FAILURE;
                current = next;
            }
            synchronized = repair_head == NullEntity;
            return synchronized;
        }

        [[nodiscard]] lux::cxx::expected<void, EHierarchyError> rebuild(
            EcsCommands commands
        ) noexcept
        {
            try
            {
                std::vector<std::pair<Entity, Entity>> edges;
                std::vector<std::pair<Entity, Entity>> repairs;
                std::size_t maximum_index{};
                for (auto [child, link] : world->query<Read<Parent>>())
                {
                    ++visited_nodes_last_update;
                    if (link.entity == NullEntity)
                    {
                        return lux::cxx::unexpected(
                            EHierarchyError::INVALID_PARENT
                        );
                    }
                    if (child == link.entity)
                    {
                        return lux::cxx::unexpected(
                            EHierarchyError::SELF_PARENT
                        );
                    }

                    const auto state =
                        detail::EcsEntityAccess::referenceState(
                            *world, link.entity
                        );
                    if (state == detail::EEntityReferenceState::UNKNOWN)
                    {
                        return lux::cxx::unexpected(
                            EHierarchyError::INVALID_PARENT
                        );
                    }
                    if (state == detail::EEntityReferenceState::STALE)
                    {
                        repairs.emplace_back(child, link.entity);
                        maximum_index = std::max(
                            maximum_index, entityIndex(child)
                        );
                        continue;
                    }
                    edges.emplace_back(child, link.entity);
                    maximum_index = std::max(
                        maximum_index,
                        std::max(entityIndex(child), entityIndex(link.entity))
                    );
                }

                std::vector<Node> next_nodes;
                if (!edges.empty() || !repairs.empty())
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
                for (const auto [child, parent] : repairs)
                {
                    Node& node = prepare_next(child);
                    node.repair_parent = parent;
                }

                // Each relation node transitions white -> gray -> black once.
                // Full rebuild therefore validates even a deep chain in O(N).
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
                    const Entity previous = parent_node->last_child;
                    child_node->previous_sibling = previous;
                    child_node->next_sibling = NullEntity;
                    if (previous == NullEntity)
                        parent_node->first_child = child;
                    else
                        find_next(previous)->next_sibling = child;
                    parent_node->last_child = child;
                }

                Entity next_repair_head = NullEntity;
                Entity next_repair_tail = NullEntity;
                for (const auto [child, parent] : repairs)
                {
                    Node* node = find_next(child);
                    detail::require(
                        node != nullptr && node->repair_parent == parent
                    );
                    node->repair_previous = next_repair_tail;
                    if (next_repair_tail == NullEntity)
                        next_repair_head = child;
                    else
                        find_next(next_repair_tail)->repair_next = child;
                    next_repair_tail = child;
                }

                nodes.swap(next_nodes);
                node_count = next_count;
                repair_head = next_repair_head;
                repair_tail = next_repair_tail;
                synchronized = repair_head == NullEntity;
                last_error = EHierarchyError::NONE;
                establishBaseline();
                if (!synchronized)
                    (void)retryRepairs(commands);
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
            EcsCommands commands
        ) noexcept
        {
            Node* node = find(entity);
            if (node == nullptr)
                return;

            cancelRepair(entity);

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
                const ECommandResult result = commands.push(
                    EraseOrphanedParent{child, entity}
                );
                if (!queueRepair(
                        child,
                        entity,
                        result == ECommandResult::ACCEPTED
                    ))
                {
                    last_error = EHierarchyError::ALLOCATION_FAILURE;
                    invalidate(EHierarchyError::ALLOCATION_FAILURE);
                    return;
                }
                if (result != ECommandResult::ACCEPTED)
                    last_error = EHierarchyError::ALLOCATION_FAILURE;
                synchronized = false;
                rebuild_required = true;
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

        void synchronize(
            EcsChangeJournal& journal,
            EcsCommands commands
        ) noexcept
        {
            visited_nodes_last_update = 0U;
            if (repair_head != NullEntity)
            {
                if (!retryRepairs(commands))
                    return;
                rebuild_required = true;
            }

            auto parent_changes = componentChanges(journal, parent_cursor);
            auto entity_changes = lux::simulation::ecs::entityChanges(
                journal,
                entity_cursor
            );
            if (parent_changes.status() == EChangeReadStatus::RESYNC_REQUIRED ||
                entity_changes.status() == EChangeReadStatus::RESYNC_REQUIRED ||
                rebuild_required || !synchronized)
            {
                auto rebuilt = rebuild(commands);
                if (!rebuilt)
                    invalidate(rebuilt.error());
                else
                    rebuild_required = false;
                return;
            }

            std::size_t maximum_index = nodes.empty() ? 0U : nodes.size() - 1U;
            for (const ComponentChange change : parent_changes)
            {
                ++visited_nodes_last_update;
                maximum_index = std::max(
                    maximum_index, entityIndex(change.entity)
                );
                const Parent* link = world->find<Parent>(change.entity);
                if (link == nullptr)
                    continue;
                const auto reference_state =
                    detail::EcsEntityAccess::referenceState(
                        *world, link->entity
                    );
                if (reference_state ==
                    detail::EEntityReferenceState::STALE)
                {
                    rebuild_required = true;
                    continue;
                }
                if (reference_state ==
                    detail::EEntityReferenceState::UNKNOWN)
                {
                    invalidate(EHierarchyError::INVALID_PARENT);
                    return;
                }
                maximum_index = std::max(
                    maximum_index, entityIndex(link->entity)
                );
                if (auto valid = validateCanonicalParent(
                        *world, change.entity, link->entity
                    ); !valid)
                {
                    invalidate(valid.error());
                    return;
                }
            }
            if (rebuild_required)
            {
                auto rebuilt = rebuild(commands);
                if (!rebuilt)
                    invalidate(rebuilt.error());
                else
                    rebuild_required = false;
                return;
            }
            if (!ensureCapacity(maximum_index))
            {
                invalidate(EHierarchyError::ALLOCATION_FAILURE);
                return;
            }

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
                if (!world->valid(change.entity))
                    continue;
                applyParent(change.entity, world->find<Parent>(change.entity));
                if (!synchronized)
                    return;
            }
            last_error = EHierarchyError::NONE;
        }

        std::vector<Node> nodes;
        std::vector<RecordedChange> changes;
        EcsState* world{};
        std::size_t node_count{};
        std::size_t change_start{};
        std::size_t change_count{};
        std::uint64_t oldest_sequence{1};
        std::uint64_t next_sequence{1};
        std::uint64_t epoch{1};
        Entity repair_head{NullEntity};
        Entity repair_tail{NullEntity};
        ChangeCursor<Parent> parent_cursor;
        EntityChangeCursor entity_cursor;
        std::size_t visited_nodes_last_update{};
        EHierarchyError last_error{EHierarchyError::NONE};
        bool synchronized{};
        bool rebuild_required{};
    };

    HierarchyIndex::HierarchyIndex(EcsState& world)
        : impl_(std::make_unique<Impl>(world))
    {
    }

    HierarchyIndex::~HierarchyIndex() noexcept = default;

    bool HierarchyIndex::boundTo(const EcsState& world) const noexcept
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

    void HierarchyIndex::synchronize(
        EcsChangeJournal& journal,
        EcsCommands commands
    ) noexcept
    {
        impl_->synchronize(journal, commands);
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
        detail::require(!impl_->changes.empty());
        const std::size_t index = (impl_->change_start + offset) %
            impl_->changes.size();
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
        EcsMutation& edit,
        Entity child,
        Entity parent
    ) noexcept
    {
        EcsState& world = detail::EcsMutationAccess::world(edit);
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
        EcsMutation& edit,
        Entity child
    ) noexcept
    {
        EcsState& world = detail::EcsMutationAccess::world(edit);
        if (!world.valid(child))
            return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);
        if (world.find<Parent>(child) != nullptr)
            edit.erase<Parent>(child);
        return {};
    }

    lux::cxx::expected<void, EHierarchyError> reparent(
        SimulationEcsMutation& mutation,
        Entity child,
        Entity parent
    ) noexcept
    {
        const EcsState& world = mutation.state();
        if (auto valid = validateCanonicalParent(world, child, parent); !valid)
            return valid;

        const Parent* current = world.find<Parent>(child);
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

    lux::cxx::expected<void, EHierarchyError> destroySubtree(
        EcsMutation& edit,
        Entity root
    ) noexcept
    {
        EcsState& world = detail::EcsMutationAccess::world(edit);
        if (!world.valid(root))
            return lux::cxx::unexpected(EHierarchyError::INVALID_ENTITY);

        try
        {
            std::vector<std::pair<Entity, Entity>> edges;
            std::size_t maximum_index{};
            for (auto [child, parent] : edit.query<Read<Parent>>())
            {
                if (parent.entity == NullEntity ||
                    !world.valid(parent.entity))
                {
                    return lux::cxx::unexpected(
                        EHierarchyError::INVALID_PARENT
                    );
                }
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
} // namespace lux::simulation::ecs
