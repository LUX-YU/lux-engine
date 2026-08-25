#include <lux/engine/ecs/TransformSystem.hpp>

#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>

#include <entt/entity/entity.hpp>

#include <algorithm>
#include <span>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        template <class Derived>
        struct RemoveDerived final
        {
            Entity entity{NullEntity};

            void apply(WorldMutation& edit) noexcept
            {
                const World& world = detail::WorldMutationAccess::world(edit);
                if (world.valid(entity) &&
                    world.find<Derived>(entity) != nullptr)
                {
                    edit.erase<Derived>(entity);
                }
            }
        };

        template <class Derived>
        struct SetDerived final
        {
            Entity entity{NullEntity};
            Derived value;

            void apply(WorldMutation& edit) noexcept
            {
                const World& world = detail::WorldMutationAccess::world(edit);
                if (!world.valid(entity))
                    return;
                if (world.find<Derived>(entity) == nullptr)
                    edit.emplace<Derived>(entity, value);
                else
                {
                    edit.update<Derived>(
                        entity,
                        [this](Derived& target) noexcept
                        {
                            target = value;
                        }
                    );
                }
            }
        };

        [[nodiscard]] std::size_t entityIndex(Entity entity) noexcept
        {
            return static_cast<std::size_t>(entt::to_entity(entity));
        }

        [[nodiscard]] Eigen::Affine2f localMatrix(
            const Transform2D& value
        ) noexcept
        {
            Eigen::Affine2f result = Eigen::Affine2f::Identity();
            result.translate(value.translation);
            result.rotate(value.rotation);
            result.scale(value.scale);
            return result;
        }

        [[nodiscard]] Eigen::Affine3f localMatrix(
            const Transform3D& value
        ) noexcept
        {
            Eigen::Affine3f result = Eigen::Affine3f::Identity();
            result.translate(value.translation);
            result.rotate(value.rotation);
            result.scale(value.scale);
            return result;
        }

        template <class Matrix>
        struct TraversalEntry final
        {
            Entity entity{NullEntity};
            Matrix parent_world{Matrix::Identity()};
            bool parent_contributes{};
        };

        template <class Local, class Derived, class Matrix>
        struct TransformState
        {
            explicit TransformState(HierarchyIndex& value) noexcept
                : hierarchy(std::addressof(value))
            {
            }

            void beginStamp()
            {
                ++stamp;
                if (stamp != 0)
                    return;
                std::fill(dirty_stamps.begin(), dirty_stamps.end(), 0U);
                std::fill(root_stamps.begin(), root_stamps.end(), 0U);
                std::fill(visited_stamps.begin(), visited_stamps.end(), 0U);
                stamp = 1U;
            }

            void ensureStamps(Entity entity)
            {
                const std::size_t required = entityIndex(entity) + 1U;
                if (dirty_stamps.size() >= required)
                    return;
                dirty_stamps.resize(required);
                root_stamps.resize(required);
                visited_stamps.resize(required);
                dirty_identities.resize(required, NullEntity);
                root_identities.resize(required, NullEntity);
                visited_identities.resize(required, NullEntity);
            }

            void mark(Entity entity)
            {
                ensureStamps(entity);
                const std::size_t index = entityIndex(entity);
                if (dirty_stamps[index] == stamp &&
                    dirty_identities[index] == entity)
                    return;
                dirty_stamps[index] = stamp;
                dirty_identities[index] = entity;
                dirty_candidates.push_back(entity);
            }

            [[nodiscard]] bool marked(Entity entity) const noexcept
            {
                if (entity == NullEntity)
                    return false;
                const std::size_t index = entityIndex(entity);
                return index < dirty_stamps.size() &&
                    dirty_stamps[index] == stamp &&
                    dirty_identities[index] == entity;
            }

            [[nodiscard]] bool visit(Entity entity)
            {
                ensureStamps(entity);
                const std::size_t index = entityIndex(entity);
                if (visited_stamps[index] == stamp &&
                    visited_identities[index] == entity)
                    return false;
                visited_stamps[index] = stamp;
                visited_identities[index] = entity;
                ++visited_nodes;
                return true;
            }

            void queueRemove(SystemContext& frame, Entity entity) noexcept
            {
                if (frame.find<Derived>(entity) == nullptr)
                    return;
                if (frame.commands().push(RemoveDerived<Derived>{entity}) !=
                    ECommandResult::ACCEPTED)
                {
                    force_resync = true;
                    ++discarded_commands;
                }
            }

            void publish(
                SystemContext& frame,
                Entity entity,
                const Matrix& value
            ) noexcept
            {
                if (frame.find<Derived>(entity) != nullptr)
                {
                    frame.update<Derived>(
                        entity,
                        [&value](Derived& target) noexcept
                        {
                            target.value = value;
                        }
                    );
                    return;
                }
                if (frame.commands().push(
                        SetDerived<Derived>{entity, Derived{value}}
                    ) != ECommandResult::ACCEPTED)
                {
                    force_resync = true;
                    ++discarded_commands;
                }
            }

            [[nodiscard]] TraversalEntry<Matrix> rootEntry(
                SystemContext& frame,
                Entity root
            )
            {
                TraversalEntry<Matrix> result;
                result.entity = root;
                Entity current = hierarchy->parent(root);
                if (current == NullEntity || !frame.valid(current) ||
                    frame.find<Local>(current) == nullptr)
                {
                    return result;
                }

                ancestors.clear();
                while (current != NullEntity && frame.valid(current) &&
                       frame.find<Local>(current) != nullptr)
                {
                    if (const Derived* derived = frame.find<Derived>(current);
                        derived != nullptr)
                    {
                        result.parent_world = derived->value;
                        result.parent_contributes = true;
                        break;
                    }
                    ancestors.push_back(current);
                    const Entity parent = hierarchy->parent(current);
                    if (parent == NullEntity || !frame.valid(parent) ||
                        frame.find<Local>(parent) == nullptr)
                    {
                        current = NullEntity;
                        break;
                    }
                    current = parent;
                }

                for (auto iterator = ancestors.rbegin();
                     iterator != ancestors.rend(); ++iterator)
                {
                    const Local* local = frame.find<Local>(*iterator);
                    detail::require(local != nullptr);
                    const Matrix value = result.parent_contributes
                        ? result.parent_world * localMatrix(*local)
                        : localMatrix(*local);
                    publish(frame, *iterator, value);
                    result.parent_world = value;
                    result.parent_contributes = true;
                    ++visited_nodes;
                }
                return result;
            }

            void traverse(SystemContext& frame, Entity root)
            {
                traversal.clear();
                traversal.push_back(rootEntry(frame, root));
                while (!traversal.empty())
                {
                    const TraversalEntry<Matrix> entry = traversal.back();
                    traversal.pop_back();
                    if (!frame.valid(entry.entity) || !visit(entry.entity))
                        continue;

                    Matrix child_world = Matrix::Identity();
                    bool child_contributes{};
                    if (const Local* local = frame.find<Local>(entry.entity);
                        local != nullptr)
                    {
                        child_world = entry.parent_contributes
                            ? entry.parent_world * localMatrix(*local)
                            : localMatrix(*local);
                        child_contributes = true;
                        publish(frame, entry.entity, child_world);
                    }
                    else
                        queueRemove(frame, entry.entity);

                    for (const Entity child :
                         hierarchy->children(entry.entity))
                    {
                        traversal.push_back(TraversalEntry<Matrix>{
                            child,
                            child_world,
                            child_contributes});
                    }
                }
            }

            void collectRoots()
            {
                roots.clear();
                for (const Entity candidate : dirty_candidates)
                {
                    if (marked(hierarchy->parent(candidate)))
                        continue;
                    ensureStamps(candidate);
                    const std::size_t index = entityIndex(candidate);
                    if (root_stamps[index] == stamp &&
                        root_identities[index] == candidate)
                        continue;
                    root_stamps[index] = stamp;
                    root_identities[index] = candidate;
                    roots.push_back(candidate);
                }
            }

            void update(SystemContext& frame) noexcept
            {
                visited_nodes = 0U;
                auto local_changes = frame.changes(local_cursor);
                auto hierarchy_changes = hierarchy->changes(hierarchy_cursor);
                if (!hierarchy->synchronized())
                {
                    force_resync = true;
                    ++invalid_hierarchy;
                    return;
                }

                const bool rebuild = force_resync ||
                    local_changes.status() ==
                        EChangeReadStatus::RESYNC_REQUIRED ||
                    hierarchy_changes.status() ==
                        EChangeReadStatus::RESYNC_REQUIRED;
                if (!rebuild && local_changes.empty() &&
                    hierarchy_changes.empty())
                {
                    return;
                }

                try
                {
                    force_resync = false;
                    beginStamp();
                    dirty_candidates.clear();
                    roots.clear();
                    if (rebuild)
                    {
                        for (auto [entity, local] :
                             frame.query<Read<Local>>())
                        {
                            (void)local;
                            mark(entity);
                        }
                        for (auto [entity, derived] :
                             frame.query<Read<Derived>>())
                        {
                            (void)derived;
                            if (frame.find<Local>(entity) == nullptr)
                                queueRemove(frame, entity);
                        }
                    }
                    else
                    {
                        for (const ComponentChange change : local_changes)
                        {
                            if (frame.valid(change.entity))
                                mark(change.entity);
                        }
                        for (const HierarchyChange change : hierarchy_changes)
                        {
                            if (frame.valid(change.entity))
                                mark(change.entity);
                        }
                    }

                    collectRoots();
                    for (const Entity root : roots)
                    {
                        if (frame.valid(root))
                            traverse(frame, root);
                    }
                }
                catch (...)
                {
                    force_resync = true;
                    ++allocation_failures;
                }
            }

            [[nodiscard]] std::size_t retainedDenseBytes() const noexcept
            {
                return (dirty_stamps.capacity() + root_stamps.capacity() +
                        visited_stamps.capacity()) * sizeof(std::uint32_t) +
                    (dirty_identities.capacity() + root_identities.capacity() +
                     visited_identities.capacity()) * sizeof(Entity);
            }

            HierarchyIndex* hierarchy{};
            ChangeCursor<Local> local_cursor;
            HierarchyChangeCursor hierarchy_cursor;
            std::vector<std::uint32_t> dirty_stamps;
            std::vector<std::uint32_t> root_stamps;
            std::vector<std::uint32_t> visited_stamps;
            std::vector<Entity> dirty_identities;
            std::vector<Entity> root_identities;
            std::vector<Entity> visited_identities;
            std::vector<Entity> dirty_candidates;
            std::vector<Entity> roots;
            std::vector<Entity> ancestors;
            std::vector<TraversalEntry<Matrix>> traversal;
            std::uint32_t stamp{};
            std::size_t visited_nodes{};
            std::size_t discarded_commands{};
            std::size_t allocation_failures{};
            std::size_t invalid_hierarchy{};
            bool force_resync{true};
        };

    } // namespace

    struct Transform2DSystem::Impl final
        : TransformState<Transform2D, WorldTransform2D, Eigen::Affine2f>
    {
        using TransformState::TransformState;
    };

    struct Transform3DSystem::Impl final
        : TransformState<Transform3D, WorldTransform3D, Eigen::Affine3f>
    {
        using TransformState::TransformState;
    };

    Transform2DSystem::Transform2DSystem(HierarchyIndex& hierarchy)
        : impl_(std::make_unique<Impl>(hierarchy))
    {
    }

    Transform2DSystem::~Transform2DSystem() = default;

    void Transform2DSystem::update(SystemContext& frame) noexcept
    {
        impl_->update(frame);
    }

    std::size_t Transform2DSystem::visitedNodesLastUpdate() const noexcept
    {
        return impl_->visited_nodes;
    }

    std::size_t Transform2DSystem::retainedDenseBytes() const noexcept
    {
        return impl_->retainedDenseBytes();
    }

    Transform3DSystem::Transform3DSystem(HierarchyIndex& hierarchy)
        : impl_(std::make_unique<Impl>(hierarchy))
    {
    }

    Transform3DSystem::~Transform3DSystem() = default;

    void Transform3DSystem::update(SystemContext& frame) noexcept
    {
        impl_->update(frame);
    }

    std::size_t Transform3DSystem::visitedNodesLastUpdate() const noexcept
    {
        return impl_->visited_nodes;
    }

    std::size_t Transform3DSystem::retainedDenseBytes() const noexcept
    {
        return impl_->retainedDenseBytes();
    }
} // namespace lux::ecs
