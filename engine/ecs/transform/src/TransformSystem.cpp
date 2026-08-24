#include <lux/engine/ecs/TransformSystem.hpp>

#include <lux/engine/ecs/Parent.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>

#include <algorithm>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        template <class Derived>
        struct RemoveDerived final
        {
            Entity entity{NullEntity};

            void apply(WorldEdit& edit) noexcept
            {
                const World& world = detail::WorldEditAccess::world(edit);
                if (world.valid(entity) && world.find<Derived>(entity) != nullptr)
                    edit.erase<Derived>(entity);
            }
        };

        template <class Derived>
        struct SetDerived final
        {
            Entity entity{NullEntity};
            Derived value;

            void apply(WorldEdit& edit) noexcept
            {
                const World& world = detail::WorldEditAccess::world(edit);
                if (!world.valid(entity))
                    return;
                if (world.find<Derived>(entity) == nullptr)
                    edit.emplace<Derived>(entity, value);
                else
                {
                    edit.update<Derived>(entity, [this](Derived& target) noexcept
                    {
                        target = value;
                    });
                }
            }
        };

        struct RemoveParent final
        {
            Entity entity{NullEntity};

            void apply(WorldEdit& edit) noexcept
            {
                const World& world = detail::WorldEditAccess::world(edit);
                if (world.valid(entity) && world.find<Parent>(entity) != nullptr)
                    edit.erase<Parent>(entity);
            }
        };

        [[nodiscard]] Eigen::Affine2f localMatrix(const Transform2D& value) noexcept
        {
            Eigen::Affine2f result = Eigen::Affine2f::Identity();
            result.translate(value.translation);
            result.rotate(value.rotation);
            result.scale(value.scale);
            return result;
        }

        [[nodiscard]] Eigen::Affine3f localMatrix(const Transform3D& value) noexcept
        {
            Eigen::Affine3f result = Eigen::Affine3f::Identity();
            result.translate(value.translation);
            result.rotate(value.rotation);
            result.scale(value.scale);
            return result;
        }

        template <class Local, class Derived, class Matrix>
        struct TransformState
        {
            explicit TransformState(HierarchyIndex& value) noexcept
                : hierarchy(std::addressof(value))
            {}

            void mark(Entity entity) noexcept
            {
                if (dirty_all)
                    return;
                try
                {
                    dirty.push_back(entity);
                }
                catch (...)
                {
                    dirty_all = true;
                    dirty.clear();
                }
            }

            [[nodiscard]] bool removeInvalidParents(
                SystemFrame& frame
            ) noexcept
            {
                bool queued{};
                for (auto [entity, parent] : frame.query<Read<Parent>>())
                {
                    if (parent.entity != entity && frame.valid(parent.entity))
                        continue;
                    if (frame.commands().push(RemoveParent{entity}) ==
                        ECommandResult::ACCEPTED)
                        queued = true;
                    else
                        ++discarded_commands;
                }
                if (queued)
                    dirty_all = true;
                return queued;
            }

            [[nodiscard]] Matrix resolve(
                SystemFrame& frame,
                Entity entity
            )
            {
                if (const auto found = computed.find(entity); found != computed.end())
                    return found->second;
                if (!visiting.insert(entity).second)
                    return Matrix::Identity();

                const Local* local = frame.find<Local>(entity);
                Matrix result = local != nullptr ? localMatrix(*local) : Matrix::Identity();
                const Entity parent = hierarchy->parent(entity);
                if (parent != NullEntity && frame.valid(parent) &&
                    frame.find<Local>(parent) != nullptr)
                {
                    Matrix parent_world;
                    if (targets.contains(parent) ||
                        frame.find<Derived>(parent) == nullptr)
                    {
                        parent_world = resolve(frame, parent);
                    }
                    else
                        parent_world = frame.get<Derived>(parent).value;
                    result = parent_world * result;
                }

                visiting.erase(entity);
                computed.insert_or_assign(entity, result);
                return result;
            }

            void update(SystemFrame& frame) noexcept
            {
                const auto parent_changes = frame.changes(parent_cursor);
                if (parent_changes.status() == EChangeReadStatus::RESYNC_REQUIRED)
                    dirty_all = true;
                else
                {
                    for (const auto& change : parent_changes)
                        mark(change.entity);
                }

                const auto local_changes = frame.changes(local_cursor);
                if (local_changes.status() == EChangeReadStatus::RESYNC_REQUIRED)
                    dirty_all = true;
                else
                {
                    for (const auto& change : local_changes)
                    {
                        mark(change.entity);
                        if (change.kind == EComponentChangeKind::REMOVED &&
                            frame.commands().push(
                                RemoveDerived<Derived>{change.entity}
                            ) != ECommandResult::ACCEPTED)
                        {
                            ++discarded_commands;
                        }
                    }
                }

                if (removeInvalidParents(frame))
                    return;
                if (!hierarchy->rebuild())
                {
                    ++invalid_hierarchy;
                    dirty_all = true;
                    return;
                }

                try
                {
                    targets.clear();
                    computed.clear();
                    visiting.clear();
                    if (dirty_all)
                    {
                        for (auto [entity, local] : frame.query<Read<Local>>())
                        {
                            (void)local;
                            targets.insert(entity);
                        }
                    }
                    else
                    {
                        for (const Entity entity : dirty)
                        {
                            if (!frame.valid(entity))
                                continue;
                            targets.insert(entity);
                            for (const Entity child : hierarchy->subtree(entity))
                                targets.insert(child);
                        }
                    }

                    for (const Entity entity : targets)
                    {
                        if (!frame.valid(entity))
                            continue;
                        const Local* local = frame.find<Local>(entity);
                        if (local == nullptr)
                            continue;
                        const Matrix value = resolve(frame, entity);
                        if (frame.find<Derived>(entity) != nullptr)
                        {
                            frame.update<Derived>(entity, [&value](Derived& target) noexcept
                            {
                                target.value = value;
                            });
                        }
                        else if (frame.commands().push(
                            SetDerived<Derived>{entity, Derived{value}}
                        ) != ECommandResult::ACCEPTED)
                        {
                            ++discarded_commands;
                        }
                    }
                    dirty.clear();
                    dirty_all = false;
                }
                catch (...)
                {
                    dirty_all = true;
                    ++allocation_failures;
                }
            }

            HierarchyIndex* hierarchy{};
            ChangeCursor<Parent> parent_cursor;
            ChangeCursor<Local> local_cursor;
            std::vector<Entity> dirty;
            std::unordered_set<Entity> targets;
            std::unordered_set<Entity> visiting;
            std::unordered_map<Entity, Matrix> computed;
            bool dirty_all{true};
            std::size_t discarded_commands{};
            std::size_t allocation_failures{};
            std::size_t invalid_hierarchy{};
        };

        template <class Local, class Derived>
        [[nodiscard]] SystemAccess transformAccess() noexcept
        {
            static const ComponentAccess components[]{
                {lux::cxx::typeToken<Parent>(), EAccessMode::READ},
                {lux::cxx::typeToken<Local>(), EAccessMode::READ},
                {lux::cxx::typeToken<Derived>(), EAccessMode::WRITE},
            };
            return SystemAccess{
                std::span<const ComponentAccess>{components},
                {},
                true,
            };
        }
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
    {}

    Transform2DSystem::~Transform2DSystem() = default;

    void Transform2DSystem::update(SystemFrame& frame) noexcept
    {
        impl_->update(frame);
    }

    SystemAccess Transform2DSystem::access() const noexcept
    {
        return transformAccess<Transform2D, WorldTransform2D>();
    }

    Transform3DSystem::Transform3DSystem(HierarchyIndex& hierarchy)
        : impl_(std::make_unique<Impl>(hierarchy))
    {}

    Transform3DSystem::~Transform3DSystem() = default;

    void Transform3DSystem::update(SystemFrame& frame) noexcept
    {
        impl_->update(frame);
    }

    SystemAccess Transform3DSystem::access() const noexcept
    {
        return transformAccess<Transform3D, WorldTransform3D>();
    }

} // namespace lux::ecs
