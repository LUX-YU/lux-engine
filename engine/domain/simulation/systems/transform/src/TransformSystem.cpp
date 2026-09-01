#include <lux/engine/simulation/systems/TransformSystem.hpp>
#include <lux/engine/simulation/systems/TransformSystem.type_static_info.hpp>

#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/ecs/hierarchy/detail/HierarchyMaintenance.hpp>

#include <entt/signal/sigh.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <vector>

namespace lux::simulation
{
    using namespace ecs;

    namespace
    {
        [[nodiscard]] Eigen::Affine2d localMatrix(const Transform2D& value) noexcept
        {
            Eigen::Affine2d result = Eigen::Affine2d::Identity();
            result.translate(value.translation);
            result.rotate(value.rotation);
            result.scale(value.scale);
            return result;
        }

        [[nodiscard]] Eigen::Affine3d localMatrix(const Transform3D& value) noexcept
        {
            Eigen::Affine3d result = Eigen::Affine3d::Identity();
            result.translate(value.translation);
            result.rotate(value.rotation);
            result.scale(value.scale);
            return result;
        }

        template <class Matrix> struct TraversalEntry final
        {
            Entity entity{NullEntity};
            Matrix parent_world{Matrix::Identity()};
            bool parent_contributes{};
        };

        template <class Local, class Derived, class Matrix> class TransformState
        {
        public:
            TransformState(Registry& registry, HierarchyIndex& hierarchy, const HierarchyDeltaBatch& hierarchy_deltas)
                : registry_(std::addressof(registry)), hierarchy_(std::addressof(hierarchy)),
                  hierarchy_deltas_(std::addressof(hierarchy_deltas)),
                  constructed_(registry.on_construct<Local>().template connect<&TransformState::onLocalChanged>(*this)),
                  updated_(registry.on_update<Local>().template connect<&TransformState::onLocalChanged>(*this)),
                  destroyed_(registry.on_destroy<Local>().template connect<&TransformState::onLocalDestroyed>(*this))
            {
            }

            [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> prepare(std::size_t entity_capacity) noexcept
            {
                try
                {
                    dirty_.clear();
                    roots_.clear();
                    ancestors_.clear();
                    traversal_.clear();
                    dirty_.reserve(entity_capacity);
                    roots_.reserve(entity_capacity);
                    ancestors_.reserve(entity_capacity);
                    traversal_.reserve(entity_capacity);
                    capacity_ = entity_capacity;
                    prepared_ = true;
                    force_resync_ = true;
                    overflowed_ = false;
                    return {};
                }
                catch (const std::bad_alloc&)
                {
                    return lux::cxx::unexpected(ETransformUpdateError::ALLOCATION_FAILURE);
                }
            }

            [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> update(EcsCommandWriter& commands) noexcept
            {
                visited_nodes_ = 0U;
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

                const bool rebuild = force_resync_ || overflowed_ || !hierarchy_deltas_->exact();
                force_resync_ = false;
                overflowed_ = false;

                if (rebuild)
                {
                    dirty_.clear();
                    for (const Entity entity : registry_->view<const Local>())
                    {
                        if (!appendDirty(entity))
                            return capacityFailure();
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
                        if (registry_->valid(delta.entity) && !appendDirty(delta.entity))
                        {
                            return capacityFailure();
                        }
                    }
                }

                if (dirty_.empty())
                    return {};

                std::sort(dirty_.begin(), dirty_.end(), [](Entity left, Entity right) noexcept {
                    return entityBits(left) < entityBits(right);
                }
                );
                dirty_.erase(std::unique(dirty_.begin(), dirty_.end()), dirty_.end());
                collectRoots();
                for (const Entity root : roots_)
                {
                    if (!registry_->valid(root))
                        continue;
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

            [[nodiscard]] std::size_t visitedNodesLastUpdate() const noexcept
            {
                return visited_nodes_;
            }

            [[nodiscard]] std::size_t retainedDenseBytes() const noexcept
            {
                return (dirty_.capacity() + roots_.capacity() + ancestors_.capacity()) * sizeof(Entity) +
                       traversal_.capacity() * sizeof(TraversalEntry<Matrix>);
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
                if (!prepared_ || dirty_.size() >= capacity_)
                {
                    overflowed_ = true;
                    force_resync_ = true;
                    return false;
                }
                dirty_.push_back(entity);
                return true;
            }

            [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError> capacityFailure() noexcept
            {
                force_resync_ = true;
                overflowed_ = true;
                dirty_.clear();
                return lux::cxx::unexpected(ETransformUpdateError::CAPACITY_EXCEEDED);
            }

            [[nodiscard]] bool isDirty(Entity entity) const noexcept
            {
                return std::binary_search(dirty_.begin(), dirty_.end(), entity, [](Entity left, Entity right) noexcept {
                    return entityBits(left) < entityBits(right);
                }
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
                        roots_.push_back(candidate);
                }
            }

            [[nodiscard]] lux::cxx::expected<TraversalEntry<Matrix>, ETransformUpdateError>
            rootEntry(Entity root, EcsCommandWriter& commands) noexcept
            {
                TraversalEntry<Matrix> result;
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
                    const Matrix value =
                        result.parent_contributes ? result.parent_world * localMatrix(local) : localMatrix(local);
                    auto published = publish(commands, *iterator, value);
                    if (!published)
                        return lux::cxx::unexpected(published.error());
                    result.parent_world = value;
                    result.parent_contributes = true;
                    ++visited_nodes_;
                }
                return result;
            }

            [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError>
            publish(EcsCommandWriter& commands, Entity entity, const Matrix& value) noexcept
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

            [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError>
            traverse(EcsCommandWriter& commands, Entity root) noexcept
            {
                traversal_.clear();
                auto entry = rootEntry(root, commands);
                if (!entry)
                    return lux::cxx::unexpected(entry.error());
                traversal_.push_back(*entry);
                while (!traversal_.empty())
                {
                    const auto current = traversal_.back();
                    traversal_.pop_back();
                    if (!registry_->valid(current.entity))
                        continue;

                    Matrix world = Matrix::Identity();
                    bool contributes{};
                    if (const auto* local = registry_->try_get<const Local>(current.entity))
                    {
                        world = current.parent_contributes ? current.parent_world * localMatrix(*local)
                                                           : localMatrix(*local);
                        contributes = true;
                        auto published = publish(commands, current.entity, world);
                        if (!published)
                            return published;
                    }
                    else if (
                        registry_->all_of<Derived>(current.entity) &&
                        !commands.template remove<Derived>(current.entity)
                    )
                    {
                        return lux::cxx::unexpected(ETransformUpdateError::COMMAND_RECORDING_FAILED);
                    }
                    ++visited_nodes_;

                    for (const Entity child : hierarchy_->children(current.entity))
                    {
                        if (traversal_.size() >= capacity_)
                        {
                            return lux::cxx::unexpected(ETransformUpdateError::CAPACITY_EXCEEDED);
                        }
                        traversal_.push_back(TraversalEntry<Matrix>{child, world, contributes});
                    }
                }
                return {};
            }

            Registry* registry_{};
            HierarchyIndex* hierarchy_{};
            const HierarchyDeltaBatch* hierarchy_deltas_{};
            std::vector<Entity> dirty_;
            std::vector<Entity> roots_;
            std::vector<Entity> ancestors_;
            std::vector<TraversalEntry<Matrix>> traversal_;
            std::size_t capacity_{};
            std::size_t visited_nodes_{};
            bool prepared_{};
            bool force_resync_{true};
            bool overflowed_{};
            entt::scoped_connection constructed_;
            entt::scoped_connection updated_;
            entt::scoped_connection destroyed_;
        };
    }

    struct Transform2DSystem::Impl final : TransformState<Transform2D, WorldTransform2D, Eigen::Affine2d>
    {
        using TransformState::TransformState;
    };

    struct Transform3DSystem::Impl final : TransformState<Transform3D, WorldTransform3D, Eigen::Affine3d>
    {
        using TransformState::TransformState;
    };

    Transform2DSystem::Transform2DSystem(
        Registry& registry,
        HierarchyIndex& hierarchy,
        const HierarchyDeltaBatch& hierarchy_deltas
    )
        : impl_(std::make_unique<Impl>(registry, hierarchy, hierarchy_deltas))
    {
    }

    Transform2DSystem::~Transform2DSystem() noexcept = default;

    lux::cxx::expected<void, ETransformUpdateError> Transform2DSystem::prepare(std::size_t entity_capacity) noexcept
    {
        return impl_->prepare(entity_capacity);
    }

    lux::cxx::expected<void, ETransformUpdateError> Transform2DSystem::update(EcsCommandWriter& commands) noexcept
    {
        return impl_->update(commands);
    }

    std::size_t Transform2DSystem::visitedNodesLastUpdate() const noexcept
    {
        return impl_->visitedNodesLastUpdate();
    }

    std::size_t Transform2DSystem::retainedDenseBytes() const noexcept
    {
        return impl_->retainedDenseBytes();
    }

    Transform3DSystem::Transform3DSystem(
        Registry& registry,
        HierarchyIndex& hierarchy,
        const HierarchyDeltaBatch& hierarchy_deltas
    )
        : impl_(std::make_unique<Impl>(registry, hierarchy, hierarchy_deltas))
    {
    }

    Transform3DSystem::~Transform3DSystem() noexcept = default;

    lux::cxx::expected<void, ETransformUpdateError> Transform3DSystem::prepare(std::size_t entity_capacity) noexcept
    {
        return impl_->prepare(entity_capacity);
    }

    lux::cxx::expected<void, ETransformUpdateError> Transform3DSystem::update(EcsCommandWriter& commands) noexcept
    {
        return impl_->update(commands);
    }

    std::size_t Transform3DSystem::visitedNodesLastUpdate() const noexcept
    {
        return impl_->visitedNodesLastUpdate();
    }

    std::size_t Transform3DSystem::retainedDenseBytes() const noexcept
    {
        return impl_->retainedDenseBytes();
    }

    namespace
    {
        inline constexpr std::array kRegisteredTransformCapabilities{
            std::string_view{"transform.2d"},
            std::string_view{"transform.3d"}
        };

        class RegisteredTransformSystem final
        {
        public:
            inline static constexpr auto Access = makeSystemAccessSpec<
                ComponentRead<Transform2D>,
                ComponentWrite<WorldTransform2D>,
                ComponentRead<Transform3D>,
                ComponentWrite<WorldTransform3D>,
                ExternalWrite<HierarchyIndex>,
                ExternalWrite<HierarchyDeltaBatch>>();
            inline static constexpr SimulationSystemDescription Description{
                .type = {
                    .canonical_name = "lux.transform",
                    .version = 1U,
                    .configuration_schema_name = "lux.transform.Configuration",
                    .configuration_schema_version = 1U,
                    .capabilities = kRegisteredTransformCapabilities
                }
            };

            explicit RegisteredTransformSystem(Registry& registry)
                : maintenance_(registry, hierarchy_, deltas_),
                  transform2d_(registry, hierarchy_, deltas_),
                  transform3d_(registry, hierarchy_, deltas_)
            {
            }

            ~RegisteredTransformSystem() noexcept = default;

            [[nodiscard]] lux::cxx::expected<void, ETransformUpdateError>
            prepare(std::size_t entity_capacity) noexcept
            {
                auto deltas = deltas_.prepare(entity_capacity);
                if (!deltas)
                    return mapHierarchyFailure(deltas.error());
                auto maintenance = maintenance_.prepare(entity_capacity);
                if (!maintenance)
                    return mapHierarchyFailure(maintenance.error());
                auto transform2d = transform2d_.prepare(entity_capacity);
                if (!transform2d)
                    return transform2d;
                return transform3d_.prepare(entity_capacity);
            }

            [[nodiscard]] bool update(EcsCommandWriter& commands) noexcept
            {
                const auto maintained = maintenance_.update();
                if (!maintained)
                    return false;
                const auto updated2d = transform2d_.update(commands);
                if (!updated2d)
                    return false;
                const auto updated3d = transform3d_.update(commands);
                return static_cast<bool>(updated3d);
            }

        private:
            [[nodiscard]] static lux::cxx::expected<void, ETransformUpdateError>
            mapHierarchyFailure(EHierarchyError error) noexcept
            {
                if (error == EHierarchyError::ALLOCATION_FAILURE)
                    return lux::cxx::unexpected(ETransformUpdateError::ALLOCATION_FAILURE);
                if (error == EHierarchyError::CAPACITY_EXCEEDED)
                    return lux::cxx::unexpected(ETransformUpdateError::CAPACITY_EXCEEDED);
                return lux::cxx::unexpected(ETransformUpdateError::INVALID_HIERARCHY);
            }

            HierarchyIndex hierarchy_;
            HierarchyDeltaBatch deltas_;
            ecs::detail::HierarchyMaintenance maintenance_;
            Transform2DSystem transform2d_;
            Transform3DSystem transform3d_;
        };

        [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> installTransformSystem(
            SimulationBuilder& builder,
            SimulationSystemView description
        ) noexcept
        {
            auto decoded = builder.decodeConfiguration<TransformSystemConfiguration>(description);
            if (!decoded)
            {
                return lux::cxx::unexpected(decoded.error());
            }
            const auto& configuration = *decoded;
            const bool values_fit = configuration.entity_capacity != 0U && configuration.max_commands != 0U &&
                configuration.max_payload_bytes != 0U &&
                configuration.entity_capacity <= (std::numeric_limits<std::size_t>::max)() &&
                configuration.max_commands <= (std::numeric_limits<std::size_t>::max)() &&
                configuration.max_payload_bytes <= (std::numeric_limits<std::size_t>::max)();
            if (!values_fit)
            {
                return lux::cxx::unexpected(SimulationSystemBuildFailure{
                    ESimulationSystemBuildError::INVALID_DESCRIPTION,
                    description.instanceId()
                });
            }

            auto system = builder.emplaceSystem<RegisteredTransformSystem>(
                description.instanceId(),
                builder.registry()
            );
            if (!system)
                return lux::cxx::unexpected(system.error());
            const auto prepared = (*system)->prepare(static_cast<std::size_t>(configuration.entity_capacity));
            if (!prepared)
            {
                const auto code = prepared.error() == ETransformUpdateError::ALLOCATION_FAILURE
                    ? ESimulationSystemBuildError::ALLOCATION_FAILURE
                    : ESimulationSystemBuildError::CONSTRUCTION_FAILURE;
                return lux::cxx::unexpected(SimulationSystemBuildFailure{code, description.instanceId()});
            }
            return builder.addSystemCommandTask<RegisteredTransformSystem>(
                description.instanceId(),
                EcsCommandProducerCapacity{
                    static_cast<std::size_t>(configuration.max_commands),
                    static_cast<std::size_t>(configuration.max_payload_bytes)
                },
                [](RegisteredTransformSystem& value, EcsCommandWriter& commands) noexcept {
                    return value.update(commands);
                }
            );
        }
    } // namespace

    const SimulationSystemDescription& transformSystemDescription() noexcept
    {
        return RegisteredTransformSystem::Description;
    }

    std::span<const SimulationSystemRegistration> transformSystemRegistrations() noexcept
    {
        static const std::array registrations{
            SimulationSystemRegistration{
                .type = lux::system::systemTypeId(RegisteredTransformSystem::Description.type.canonical_name),
                .cpp_type = lux::cxx::typeToken<RegisteredTransformSystem>(),
                .description = &RegisteredTransformSystem::Description,
                .access = RegisteredTransformSystem::Access.spec(),
                .configuration = lux::serialization::makePortableValueCodec<TransformSystemConfiguration>(),
                .install = &installTransformSystem
            }
        };
        return registrations;
    }

    lux::cxx::expected<std::vector<std::byte>, ETransformUpdateError> makeTransformSystemConfiguration(
        std::size_t entity_capacity,
        EcsCommandProducerCapacity command_capacity
    ) noexcept
    {
        if (entity_capacity == 0U || command_capacity.max_commands == 0U ||
            command_capacity.max_payload_bytes == 0U)
        {
            return lux::cxx::unexpected(ETransformUpdateError::CAPACITY_EXCEEDED);
        }
        const TransformSystemConfiguration configuration{
            static_cast<std::uint64_t>(entity_capacity),
            static_cast<std::uint64_t>(command_capacity.max_commands),
            static_cast<std::uint64_t>(command_capacity.max_payload_bytes)
        };
        std::vector<std::byte> result;
        const auto encoded = lux::serialization::makePortableValueCodec<TransformSystemConfiguration>().encode(
            &configuration,
            result
        );
        if (!encoded)
        {
            return lux::cxx::unexpected(ETransformUpdateError::ALLOCATION_FAILURE);
        }
        return result;
    }
}
