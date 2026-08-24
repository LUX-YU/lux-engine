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
                World& world = detail::WorldEditAccess::world(edit);
                if (world.valid(entity) && world.find<Derived>(entity) != nullptr)
                    edit.remove<Derived>(entity);
            }
        };

        template <class Derived>
        struct SetDerived final
        {
            Entity entity{NullEntity};
            Derived value;

            void apply(WorldEdit& edit) noexcept
            {
                World& world = detail::WorldEditAccess::world(edit);
                if (!world.valid(entity))
                    return;
                if (world.find<Derived>(entity) == nullptr)
                    edit.emplace<Derived>(entity, value);
                else
                {
                    world.patch<Derived>(entity, [this](Derived& target) noexcept
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
                World& world = detail::WorldEditAccess::world(edit);
                if (world.valid(entity) && world.find<Parent>(entity) != nullptr)
                    edit.remove<Parent>(entity);
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
                World& world,
                WorldCommands commands
            ) noexcept
            {
                bool queued{};
                for (auto [entity, parent] : world.view<const Parent>().each())
                {
                    if (parent.entity != entity && world.valid(parent.entity))
                        continue;
                    if (commands.push(RemoveParent{entity}) == ECommandResult::ACCEPTED)
                        queued = true;
                    else
                        ++discarded_commands;
                }
                if (queued)
                    dirty_all = true;
                return queued;
            }

            [[nodiscard]] Matrix resolve(
                World& world,
                Entity entity
            )
            {
                if (const auto found = computed.find(entity); found != computed.end())
                    return found->second;
                if (!visiting.insert(entity).second)
                    return Matrix::Identity();

                const Local* local = world.find<Local>(entity);
                Matrix result = local != nullptr ? localMatrix(*local) : Matrix::Identity();
                const Entity parent = hierarchy->parent(entity);
                if (parent != NullEntity && world.valid(parent) &&
                    world.find<Local>(parent) != nullptr)
                {
                    Matrix parent_world;
                    if (targets.contains(parent) ||
                        world.find<Derived>(parent) == nullptr)
                    {
                        parent_world = resolve(world, parent);
                    }
                    else
                        parent_world = world.get<Derived>(parent).value;
                    result = parent_world * result;
                }

                visiting.erase(entity);
                computed.insert_or_assign(entity, result);
                return result;
            }

            void update(const SystemFrame& frame) noexcept
            {
                World& world = frame.world();
                if (removeInvalidParents(world, frame.commands()))
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
                        for (const Entity entity : world.view<const Local>())
                            targets.insert(entity);
                    }
                    else
                    {
                        for (const Entity entity : dirty)
                        {
                            if (!world.valid(entity))
                                continue;
                            targets.insert(entity);
                            for (const Entity child : hierarchy->subtree(entity))
                                targets.insert(child);
                        }
                    }

                    for (const Entity entity : targets)
                    {
                        if (!world.valid(entity))
                            continue;
                        const Local* local = world.find<Local>(entity);
                        if (local == nullptr)
                            continue;
                        const Matrix value = resolve(world, entity);
                        if (world.find<Derived>(entity) != nullptr)
                        {
                            world.patch<Derived>(entity, [&value](Derived& target) noexcept
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
            WorldCommands observer_commands;
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
                true,
            };
        }

        [[nodiscard]] std::span<const SystemSetId> transformSets() noexcept
        {
            static constexpr SystemSetId values[]{
                systemSetId("lux.ecs.transform.resolve"),
            };
            return values;
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

    void Transform2DSystem::onAttach(SystemAttach& attach) noexcept
    {
        detail::require(impl_->hierarchy->boundTo(attach.world()));
        impl_->observer_commands = attach.commands();
        attach.observeConstruct<Parent, &Transform2DSystem::onHierarchyChanged>(*this);
        attach.observeUpdate<Parent, &Transform2DSystem::onHierarchyChanged>(*this);
        attach.observeDestroy<Parent, &Transform2DSystem::onHierarchyChanged>(*this);
        attach.observeConstruct<Transform2D, &Transform2DSystem::onLocalChanged>(*this);
        attach.observeUpdate<Transform2D, &Transform2DSystem::onLocalChanged>(*this);
        attach.observeDestroy<Transform2D, &Transform2DSystem::onLocalDestroyed>(*this);
        impl_->dirty_all = true;
    }

    void Transform2DSystem::onDetach(SystemDetach&) noexcept
    {
        impl_->observer_commands = {};
        impl_->dirty.clear();
        impl_->targets.clear();
        impl_->computed.clear();
        impl_->visiting.clear();
    }

    void Transform2DSystem::update(const SystemFrame& frame) noexcept
    {
        impl_->update(frame);
    }

    SystemAccess Transform2DSystem::access() const noexcept
    {
        return transformAccess<Transform2D, WorldTransform2D>();
    }

    std::span<const SystemSetId> Transform2DSystem::sets() const noexcept
    {
        return transformSets();
    }

    void Transform2DSystem::onHierarchyChanged(Entity entity) noexcept
    {
        impl_->mark(entity);
    }

    void Transform2DSystem::onLocalChanged(Entity entity) noexcept
    {
        impl_->mark(entity);
    }

    void Transform2DSystem::onLocalDestroyed(Entity entity) noexcept
    {
        impl_->mark(entity);
        if (impl_->observer_commands.push(
            RemoveDerived<WorldTransform2D>{entity}
        ) != ECommandResult::ACCEPTED)
        {
            ++impl_->discarded_commands;
        }
    }

    Transform3DSystem::Transform3DSystem(HierarchyIndex& hierarchy)
        : impl_(std::make_unique<Impl>(hierarchy))
    {}

    Transform3DSystem::~Transform3DSystem() = default;

    void Transform3DSystem::onAttach(SystemAttach& attach) noexcept
    {
        detail::require(impl_->hierarchy->boundTo(attach.world()));
        impl_->observer_commands = attach.commands();
        attach.observeConstruct<Parent, &Transform3DSystem::onHierarchyChanged>(*this);
        attach.observeUpdate<Parent, &Transform3DSystem::onHierarchyChanged>(*this);
        attach.observeDestroy<Parent, &Transform3DSystem::onHierarchyChanged>(*this);
        attach.observeConstruct<Transform3D, &Transform3DSystem::onLocalChanged>(*this);
        attach.observeUpdate<Transform3D, &Transform3DSystem::onLocalChanged>(*this);
        attach.observeDestroy<Transform3D, &Transform3DSystem::onLocalDestroyed>(*this);
        impl_->dirty_all = true;
    }

    void Transform3DSystem::onDetach(SystemDetach&) noexcept
    {
        impl_->observer_commands = {};
        impl_->dirty.clear();
        impl_->targets.clear();
        impl_->computed.clear();
        impl_->visiting.clear();
    }

    void Transform3DSystem::update(const SystemFrame& frame) noexcept
    {
        impl_->update(frame);
    }

    SystemAccess Transform3DSystem::access() const noexcept
    {
        return transformAccess<Transform3D, WorldTransform3D>();
    }

    std::span<const SystemSetId> Transform3DSystem::sets() const noexcept
    {
        return transformSets();
    }

    void Transform3DSystem::onHierarchyChanged(Entity entity) noexcept
    {
        impl_->mark(entity);
    }

    void Transform3DSystem::onLocalChanged(Entity entity) noexcept
    {
        impl_->mark(entity);
    }

    void Transform3DSystem::onLocalDestroyed(Entity entity) noexcept
    {
        impl_->mark(entity);
        if (impl_->observer_commands.push(
            RemoveDerived<WorldTransform3D>{entity}
        ) != ECommandResult::ACCEPTED)
        {
            ++impl_->discarded_commands;
        }
    }
} // namespace lux::ecs
