#include <lux/engine/SceneSystemRegistration.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/TransformSystemConfiguration.type_static_info.hpp>
#include <lux/engine/ecs/TransformEvaluation.hpp>
#include <lux/engine/ecs/hierarchy/detail/HierarchyMaintenance.hpp>
#include <lux/engine/world/World.hpp>

#include <algorithm>
#include <limits>
#include <lux/engine/error/ErrorRegistry.hpp>

namespace lux::scene
{
    namespace
    {
        namespace Errors
        {
            constexpr error::ErrorDescriptor SceneTransformUpdateDescriptor{
                "lux.scene.transform.update",
                "Transform update code {0}",
                error::ERecovery::NEEDS_INPUT,
                {error::EArgument::UNSIGNED}
            };
            constexpr error::ErrorId SceneTransformUpdate = error::errorId(SceneTransformUpdateDescriptor.name);
            constexpr error::ErrorDescriptor SceneTransformHierarchyDescriptor{
                "lux.scene.transform.hierarchy",
                "Hierarchy maintenance code {0}",
                error::ERecovery::NEEDS_INPUT,
                {error::EArgument::UNSIGNED}
            };
            constexpr error::ErrorId SceneTransformHierarchy = error::errorId(SceneTransformHierarchyDescriptor.name);
            constexpr error::ErrorDescriptor SceneTransformCommandsDescriptor{
                "lux.scene.transform.commands",
                "ECS command code {0}; producer {1}, command {2}",
                error::ERecovery::NEEDS_INPUT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}
            };
            constexpr error::ErrorId SceneTransformCommands = error::errorId(SceneTransformCommandsDescriptor.name);
        } // namespace Errors
        constexpr error::ErrorDescriptor ErrorDescriptors[]{
            Errors::SceneTransformUpdateDescriptor,
            Errors::SceneTransformHierarchyDescriptor,
            Errors::SceneTransformCommandsDescriptor
        };
    } // namespace

    using namespace ecs;
    namespace
    {
        error::Error transformError(ETransformUpdateError value) noexcept
        {
            return error::Error{Errors::SceneTransformUpdate, {static_cast<std::uint64_t>(value)}};
        }
        error::Error transformError(EHierarchyError value) noexcept
        {
            return error::Error{Errors::SceneTransformHierarchy, {static_cast<std::uint64_t>(value)}};
        }
        error::Error transformError(EcsCommandFailure value) noexcept
        {
            return error::Error{
                Errors::SceneTransformCommands,
                {static_cast<std::uint64_t>(value.code), value.producer, value.command}
            };
        }

        template <class Matrix> struct TTraversalEntry final
        {
            Entity entity{NullEntity};
            Matrix parent_world{Matrix::Identity()};
            bool parent_contributes{};
        };

        template <class Local, class Derived, class Matrix> class TTransformState
        {
        public:
            TTransformState(Registry& registry, HierarchyIndex& hierarchy, const HierarchyDeltaBatch& hierarchy_deltas)
                : registry_(std::addressof(registry)), hierarchy_(std::addressof(hierarchy)),
                  hierarchy_deltas_(std::addressof(hierarchy_deltas)),
                  constructed_(registry.on_construct<Local>().template connect<&TTransformState::onLocalChanged>(*this)
                  ),
                  updated_(registry.on_update<Local>().template connect<&TTransformState::onLocalChanged>(*this)),
                  destroyed_(registry.on_destroy<Local>().template connect<&TTransformState::onLocalDestroyed>(*this))
            {
            }

            [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> prepare(std::size_t entity_capacity) noexcept
            {
                dirty_.clear();
                pending_.clear();
                roots_.clear();
                ancestors_.clear();
                traversal_.clear();
                dirty_.reserve(entity_capacity * 2);
                pending_.reserve(entity_capacity * 2);
                roots_.reserve(entity_capacity);
                ancestors_.reserve(entity_capacity);
                traversal_.reserve(entity_capacity);
                capacity_ = entity_capacity;
                prepared_ = true;
                force_resync_ = true;
                return {};
            }

            void beginUpdate() noexcept
            {
                dirty_.clear();
                dirty_.swap(pending_);
                rebuild_ = force_resync_;
                force_resync_ = false;
            }

            [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> update(EcsCommandWriter& commands) noexcept
            {
                if (!prepared_)
                {
                    return lux::cxx::unexpected(ETransformUpdateError::CAPACITY_EXCEEDED);
                }
                if (!hierarchy_->synchronized())
                {
                    force_resync_ = true;
                    return lux::cxx::unexpected(ETransformUpdateError::INVALID_HIERARCHY);
                }
                if (!commands)
                {
                    force_resync_ = true;
                    return lux::cxx::unexpected(ETransformUpdateError::COMMAND_RECORDING_FAILED);
                }

                const bool rebuild = rebuild_ || !hierarchy_deltas_->exact();

                if (rebuild)
                {
                    dirty_.clear();
                    for (const Entity entity : registry_->view<const Local>())
                    {
                        if (!appendCurrent(entity))
                        {
                            return capacityFailure();
                        }
                    }
                    for (const Entity entity : registry_->view<const Derived>())
                    {
                        if (!registry_->all_of<Local>(entity) && !commands.template remove<Derived>(entity))
                        {
                            force_resync_ = true;
                            return lux::cxx::unexpected(ETransformUpdateError::COMMAND_RECORDING_FAILED);
                        }
                    }
                }
                else
                {
                    for (const HierarchyDelta delta : hierarchy_deltas_->values())
                    {
                        if (registry_->valid(delta.entity) && !appendCurrent(delta.entity))
                        {
                            return capacityFailure();
                        }
                    }
                }

                if (dirty_.empty())
                {
                    return {};
                }

                std::sort(
                    dirty_.begin(),
                    dirty_.end(),
                    [](Entity left, Entity right) noexcept { return entityBits(left) < entityBits(right); }
                );
                dirty_.erase(std::unique(dirty_.begin(), dirty_.end()), dirty_.end());
                if (dirty_.size() > capacity_)
                {
                    return capacityFailure();
                }
                collectRoots();
                for (const Entity root : roots_)
                {
                    if (!registry_->valid(root))
                    {
                        continue;
                    }
                    auto traversed = traverse(commands, root);
                    if (!traversed)
                    {
                        force_resync_ = true;
                        return traversed;
                    }
                }
                dirty_.clear();
                return {};
            }

            [[nodiscard]] bool hasPendingChanges() const noexcept
            {
                return force_resync_ || !pending_.empty();
            }

        private:
            void onLocalChanged(Registry&, Entity entity) noexcept
            {
                (void)appendDirty(entity);
            }

            void onLocalDestroyed(Registry&, Entity entity) noexcept
            {
                (void)appendDirty(entity);
            }

            [[nodiscard]] bool appendDirty(Entity entity) noexcept
            {
                if (!prepared_ || pending_.size() >= capacity_)
                {
                    force_resync_ = true;
                    return false;
                }
                pending_.push_back(entity);
                return true;
            }

            [[nodiscard]] bool appendCurrent(Entity entity) noexcept
            {
                if (dirty_.size() >= capacity_ * 2)
                {
                    return false;
                }
                dirty_.push_back(entity);
                return true;
            }

            [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> capacityFailure() noexcept
            {
                force_resync_ = true;
                dirty_.clear();
                return lux::cxx::unexpected(ETransformUpdateError::CAPACITY_EXCEEDED);
            }

            [[nodiscard]] bool isDirty(Entity entity) const noexcept
            {
                return std::binary_search(
                    dirty_.begin(),
                    dirty_.end(),
                    entity,
                    [](Entity left, Entity right) noexcept { return entityBits(left) < entityBits(right); }
                );
            }

            void collectRoots() noexcept
            {
                roots_.clear();
                for (const Entity candidate : dirty_)
                {
                    Entity parent = hierarchy_->parent(candidate);
                    bool covered{};
                    while (parent != NullEntity && registry_->valid(parent))
                    {
                        if (isDirty(parent))
                        {
                            covered = true;
                            break;
                        }
                        parent = hierarchy_->parent(parent);
                    }
                    if (!covered)
                    {
                        roots_.push_back(candidate);
                    }
                }
            }

            [[nodiscard]] lux::cxx::expected<TTraversalEntry<Matrix>, ETransformUpdateError> rootEntry(
                Entity root,
                EcsCommandWriter& commands
            ) noexcept
            {
                TTraversalEntry<Matrix> result;
                result.entity = root;
                Entity current = hierarchy_->parent(root);
                if (current == NullEntity || !registry_->valid(current) || !registry_->all_of<Local>(current))
                {
                    return result;
                }

                if (const auto* derived = registry_->try_get<Derived>(current))
                {
                    result.parent_world = derived->value;
                    result.parent_contributes = true;
                    return result;
                }

                ancestors_.clear();
                while (current != NullEntity && registry_->valid(current) && registry_->all_of<Local>(current))
                {
                    if (ancestors_.size() >= capacity_)
                    {
                        return lux::cxx::unexpected(ETransformUpdateError::CAPACITY_EXCEEDED);
                    }
                    ancestors_.push_back(current);
                    const Entity parent = hierarchy_->parent(current);
                    if (parent == NullEntity || !registry_->valid(parent) || !registry_->all_of<Local>(parent))
                    {
                        break;
                    }
                    if (const auto* derived = registry_->try_get<Derived>(parent))
                    {
                        result.parent_world = derived->value;
                        result.parent_contributes = true;
                        break;
                    }
                    current = parent;
                }

                for (auto iterator = ancestors_.rbegin(); iterator != ancestors_.rend(); ++iterator)
                {
                    const auto& local = registry_->get<const Local>(*iterator);
                    const Matrix value = result.parent_contributes ? result.parent_world * localTransformMatrix(local)
                                                                   : localTransformMatrix(local);
                    auto published = publish(commands, *iterator, value);
                    if (!published)
                    {
                        return lux::cxx::unexpected(published.error());
                    }
                    result.parent_world = value;
                    result.parent_contributes = true;
                }
                return result;
            }

            [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> publish(
                EcsCommandWriter& commands,
                Entity entity,
                const Matrix& value
            ) noexcept
            {
                if (registry_->all_of<Derived>(entity))
                {
                    registry_->patch<Derived>(entity, [&value](Derived& target) noexcept { target.value = value; });
                    return {};
                }
                if (!commands.template emplace<Derived>(entity, Derived{value}))
                {
                    return lux::cxx::unexpected(ETransformUpdateError::COMMAND_RECORDING_FAILED);
                }
                return {};
            }

            [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> traverse(
                EcsCommandWriter& commands,
                Entity root
            ) noexcept
            {
                traversal_.clear();
                auto entry = rootEntry(root, commands);
                if (!entry)
                {
                    return lux::cxx::unexpected(entry.error());
                }
                traversal_.push_back(*entry);
                while (!traversal_.empty())
                {
                    const auto current = traversal_.back();
                    traversal_.pop_back();
                    if (!registry_->valid(current.entity))
                    {
                        continue;
                    }

                    Matrix world = Matrix::Identity();
                    bool contributes{};
                    if (const auto* local = registry_->try_get<const Local>(current.entity))
                    {
                        world = current.parent_contributes ? current.parent_world * localTransformMatrix(*local)
                                                           : localTransformMatrix(*local);
                        contributes = true;
                        auto published = publish(commands, current.entity, world);
                        if (!published)
                        {
                            return published;
                        }
                    }
                    else if (registry_->all_of<Derived>(current.entity) &&
                             !commands.template remove<Derived>(current.entity))
                    {
                        return lux::cxx::unexpected(ETransformUpdateError::COMMAND_RECORDING_FAILED);
                    }

                    for (const Entity child : hierarchy_->children(current.entity))
                    {
                        if (traversal_.size() >= capacity_)
                        {
                            return lux::cxx::unexpected(ETransformUpdateError::CAPACITY_EXCEEDED);
                        }
                        traversal_.push_back(TTraversalEntry<Matrix>{child, world, contributes});
                    }
                }
                return {};
            }

            Registry* registry_{};
            HierarchyIndex* hierarchy_{};
            const HierarchyDeltaBatch* hierarchy_deltas_{};
            std::vector<Entity> dirty_;
            std::vector<Entity> pending_;
            std::vector<Entity> roots_;
            std::vector<Entity> ancestors_;
            std::vector<TTraversalEntry<Matrix>> traversal_;
            std::size_t capacity_{};
            bool rebuild_{};
            bool prepared_{};
            bool force_resync_{true};
            entt::scoped_connection constructed_;
            entt::scoped_connection updated_;
            entt::scoped_connection destroyed_;
        };
    } // namespace

    struct TransformSystem::Impl final
    {
        explicit Impl(Registry& value)
            : maintenance(value, hierarchy, deltas), transform2d(value, hierarchy, deltas),
              transform3d(value, hierarchy, deltas)
        {
        }

        template <class Error> [[nodiscard]] static Result failure(Error error) noexcept
        {
            return cxx::unexpected(transformError(error));
        }

        [[nodiscard]] Result prepare(const TransformSystemConfiguration& configuration) noexcept
        {
            const bool has_zero_capacity = configuration.entity_capacity == 0 || configuration.max_commands == 0 ||
                                           configuration.max_payload_bytes == 0;
            const bool is_capacity_overflow =
                configuration.entity_capacity > std::numeric_limits<std::size_t>::max() / 2 ||
                configuration.max_commands > std::numeric_limits<std::size_t>::max() ||
                configuration.max_payload_bytes > std::numeric_limits<std::size_t>::max();
            const bool is_invalid_capacity = has_zero_capacity || is_capacity_overflow;
            if (is_invalid_capacity)
            {
                return failure(ETransformUpdateError::CAPACITY_EXCEEDED);
            }
            const auto capacity = static_cast<std::size_t>(configuration.entity_capacity);
            if (const auto result = deltas.prepare(capacity); !result)
            {
                return failure(result.error());
            }
            if (const auto result = maintenance.prepare(capacity); !result)
            {
                return failure(result.error());
            }
            if (const auto result = transform2d.prepare(capacity); !result)
            {
                return failure(result.error());
            }
            if (const auto result = transform3d.prepare(capacity); !result)
            {
                return failure(result.error());
            }
            producer = EcsCommandProducerCapacity{
                static_cast<std::size_t>(configuration.max_commands),
                static_cast<std::size_t>(configuration.max_payload_bytes)
            };
            return {};
        }

        [[nodiscard]] bool hasPendingChanges() const noexcept
        {
            return maintenance.hasPendingChanges() || transform2d.hasPendingChanges() || transform3d.hasPendingChanges();
        }

        [[nodiscard]] Result synchronize(EcsCommandWriter& writer) noexcept
        {
            if (const auto result = maintenance.update(writer); !result)
            {
                return failure(result.error());
            }
            transform2d.beginUpdate();
            transform3d.beginUpdate();
            if (const auto result = transform2d.update(writer); !result)
            {
                return failure(result.error());
            }
            if (const auto result = transform3d.update(writer); !result)
            {
                return failure(result.error());
            }
            return {};
        }

        HierarchyIndex hierarchy;
        HierarchyDeltaBatch deltas;
        EcsCommandProducerCapacity producer{};
        ecs::detail::HierarchyMaintenance maintenance;
        TTransformState<Transform2D, WorldTransform2D, Eigen::Affine2d> transform2d;
        TTransformState<Transform3D, WorldTransform3D, Eigen::Affine3d> transform3d;
    };

    TransformSystem::TransformSystem(Registry& registry) : impl_(std::make_unique<Impl>(registry)) {}

    TransformSystem::~TransformSystem() noexcept = default;

    SceneSystem::Result TransformSystem::prepare(const TransformSystemConfiguration& configuration) noexcept
    {
        if (auto registered = error::ErrorRegistry::instance().registerTypes(ErrorDescriptors); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        return impl_->prepare(configuration);
    }
    SceneSystem::Result TransformSystem::execute() noexcept
    {
        if (!impl_->hasPendingChanges())
        {
            return {};
        }
        auto writer = beginCommands();
        return writer ? impl_->synchronize(*writer) : Impl::failure(writer.error());
    }

    EcsCommandProducerCapacity TransformSystem::commandCapacity() const noexcept
    {
        return impl_->producer;
    }

    SceneSystemRegistration transformSystemRegistration() noexcept
    {
        return {
            .name = std::string{TransformSystem::Name},
            .configuration_version = 1,
            .configuration = lux::serialization::makePortableValueCodec<TransformSystemConfiguration>(),
            .create = +[](engine::EngineContext&, world::World& world, simulation::Simulation&,
                          const ecs::ComponentSchemaSet&, std::span<const std::byte> bytes
            ) noexcept -> cxx::expected<std::unique_ptr<SceneSystem>, error::Error>
            {
                TransformSystemConfiguration config{};
                const auto codec = serialization::makePortableValueCodec<TransformSystemConfiguration>();
                if (!codec.decode(bytes, &config))
                {
                    return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_ARGUMENT));
                }
                auto system = std::make_unique<TransformSystem>(world.registry());
                if (auto prepared = system->prepare(config); !prepared)
                {
                    return cxx::unexpected(prepared.error());
                }
                return std::unique_ptr<SceneSystem>{std::move(system)};
            }
        };
    }

    lux::cxx::expected<std::vector<std::byte>, ETransformUpdateError> makeTransformSystemConfiguration(
        std::size_t entity_capacity,
        EcsCommandProducerCapacity command_capacity
    ) noexcept
    {
        if (entity_capacity == 0U || command_capacity.max_commands == 0U || command_capacity.max_payload_bytes == 0U)
        {
            return lux::cxx::unexpected(ETransformUpdateError::CAPACITY_EXCEEDED);
        }
        const TransformSystemConfiguration configuration{
            static_cast<std::uint64_t>(entity_capacity),
            static_cast<std::uint64_t>(command_capacity.max_commands),
            static_cast<std::uint64_t>(command_capacity.max_payload_bytes)
        };
        std::vector<std::byte> result;
        const auto encoded =
            lux::serialization::makePortableValueCodec<TransformSystemConfiguration>().encode(&configuration, result);
        if (!encoded)
        {
            return lux::cxx::unexpected(ETransformUpdateError::CONFIGURATION_ENCODE_FAILURE);
        }
        return result;
    }
} // namespace lux::scene
