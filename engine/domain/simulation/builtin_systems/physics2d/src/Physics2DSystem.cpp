#include <lux/engine/physics2d/Physics2DSystem.hpp>

#include "PhysicsQuery2D.ability.generated.hpp"
#include <lux/engine/physics2d/Box2DWorld.hpp>
#include <lux/engine/physics2d/Physics2DSystem.type_static_info.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <lux/engine/simulation/SimulationSystemInstaller.hpp>

#include <entt/container/dense_map.hpp>

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace lux::physics2d
{
    namespace
    {
        constexpr double kMaximumRelativeCoordinate = 1'000'000.0;

        struct BodyRecord final
        {
            detail::Box2DWorld::BodyId body{};
            bool dynamic{};
        };

        [[nodiscard]] bool validConfiguration(const Physics2DSystemConfiguration& value) noexcept
        {
            const bool invalid_gravity = !std::isfinite(value.gravity_x) || !std::isfinite(value.gravity_y) ||
                                         std::abs(value.gravity_x) > std::numeric_limits<float>::max() ||
                                         std::abs(value.gravity_y) > std::numeric_limits<float>::max();
            const bool invalid_fixed_step = value.fixed_step_nanoseconds <= 0;
            const bool invalid_substeps = value.max_substeps == 0U;
            const bool invalid_capacity = value.body_capacity == 0U ||
                                          value.body_capacity > std::numeric_limits<detail::Box2DWorld::BodyId>::max();
            const bool accumulated_overflow =
                !invalid_fixed_step && !invalid_substeps &&
                value.fixed_step_nanoseconds > std::numeric_limits<std::int64_t>::max() / value.max_substeps;
            return !invalid_gravity && !invalid_fixed_step && !invalid_substeps && !invalid_capacity &&
                   !accumulated_overflow;
        }

        [[nodiscard]] lux::cxx::expected<void, lux::simulation::SimulationSystemBuildFailure> installPhysics2DSystem(
            lux::simulation::SimulationSystemInstaller& builder,
            lux::simulation::SimulationSystemView description
        ) noexcept
        {
            auto configuration = builder.decodeConfiguration<Physics2DSystemConfiguration>(description);
            if (!configuration)
            {
                return lux::cxx::unexpected(lux::simulation::SimulationSystemBuildFailure{
                    lux::simulation::ESimulationSystemBuildError::CONFIGURATION_DECODE_FAILURE,
                    description.instanceId()
                });
            }
            auto prepared = Physics2DSystem::create(builder.registry(), builder.time(), *configuration);
            if (!prepared)
            {
                const auto code = prepared.error() == EPhysics2DSystemError::INVALID_CONFIGURATION
                                      ? lux::simulation::ESimulationSystemBuildError::CONFIGURATION_DECODE_FAILURE
                                      : lux::simulation::ESimulationSystemBuildError::CONSTRUCTION_FAILURE;
                return lux::cxx::unexpected(
                    lux::simulation::SimulationSystemBuildFailure{code, description.instanceId()}
                );
            }
            auto system = builder.addSystem(description.instanceId(), std::move(*prepared));
            if (!system)
            {
                return lux::cxx::unexpected(system.error());
            }
            const auto task = builder.addSystemTask<Physics2DSystem>(
                description.instanceId(),
                [](Physics2DSystem& value) noexcept { return value.update(); }
            );
            if (!task)
            {
                return lux::cxx::unexpected(task.error());
            }
            return builder.publishScriptAbility(
                description.instanceId(),
                lux::script::bindScriptAbility<PhysicsQuery2D>(**system)
            );
        }
    } // namespace

    struct Physics2DSystem::Impl final
    {
        lux::simulation::ecs::Registry& registry;
        const lux::simulation::SimulationTime& time;
        Physics2DSystemConfiguration configuration;
        detail::Box2DWorld world;
        entt::dense_map<lux::simulation::ecs::Entity, BodyRecord> bodies;
        std::optional<Eigen::Vector2d> origin;
        lux::simulation::SimulationDuration accumulator{};
        std::uint64_t completed_steps{};
        std::uint64_t overlap_queries{};

        Impl(
            lux::simulation::ecs::Registry& source_registry,
            const lux::simulation::SimulationTime& source_time,
            Physics2DSystemConfiguration source_configuration,
            detail::Box2DWorld prepared_world
        )
            : registry(source_registry), time(source_time), configuration(source_configuration),
              world(std::move(prepared_world))
        {
        }

        [[nodiscard]] std::optional<Eigen::Vector2f> relative(Eigen::Vector2d value) const noexcept
        {
            if (!origin || !value.allFinite())
            {
                return std::nullopt;
            }
            const auto offset = value - *origin;
            if (!offset.allFinite() || offset.cwiseAbs().maxCoeff() > kMaximumRelativeCoordinate)
            {
                return std::nullopt;
            }
            return offset.cast<float>();
        }

        [[nodiscard]] bool syncBodies() noexcept
        {
            using lux::simulation::ecs::Transform2D;
            if (!origin)
            {
                const auto view = registry.view<const BoxCollider2D, const Transform2D>();
                for (const auto entity : view)
                {
                    const auto& transform = view.template get<const Transform2D>(entity);
                    if (transform.translation.allFinite())
                    {
                        origin = transform.translation;
                        break;
                    }
                }
            }

            bool success = true;
            registry.view<const BoxCollider2D, const Transform2D>().each(
                [&](lux::simulation::ecs::Entity entity, const BoxCollider2D& collider, const Transform2D& transform)
                {
                    if (!success)
                    {
                        return;
                    }
                    const bool dynamic = registry.all_of<RigidBody2D>(entity);
                    const auto rotation = Eigen::Rotation2Dd{transform.rotation};
                    const auto scaled_offset = collider.offset.cwiseProduct(transform.scale);
                    const auto center = transform.translation + rotation * scaled_offset;
                    const auto half = collider.half_extents.cwiseProduct(transform.scale.cwiseAbs());
                    const auto relative_center = relative(center);
                    const bool invalid = !relative_center || !half.allFinite() || half.minCoeff() <= 0.0 ||
                                         half.maxCoeff() > std::numeric_limits<float>::max() ||
                                         !std::isfinite(transform.rotation);
                    if (invalid)
                    {
                        success = false;
                        return;
                    }

                    auto found = bodies.find(entity);
                    if (found != bodies.end() && found->second.dynamic != dynamic)
                    {
                        world.destroyBody(found->second.body);
                        bodies.erase(found);
                        found = bodies.end();
                    }
                    if (found == bodies.end())
                    {
                        if (bodies.size() >= configuration.body_capacity)
                        {
                            success = false;
                            return;
                        }
                        const auto body = world.createBox(
                            *relative_center,
                            static_cast<float>(transform.rotation),
                            half.cast<float>(),
                            dynamic
                        );
                        if (!body)
                        {
                            success = false;
                            return;
                        }
                        found = bodies.emplace(entity, BodyRecord{*body, dynamic}).first;
                        if (dynamic)
                        {
                            const auto& state = registry.get<RigidBody2D>(entity);
                            if (!state.velocity.allFinite() || !std::isfinite(state.gravity_scale))
                            {
                                success = false;
                                return;
                            }
                            world.setLinearVelocity(found->second.body, state.velocity.cast<float>());
                            world.setGravityScale(found->second.body, static_cast<float>(state.gravity_scale));
                        }
                    }
                    else if (!dynamic)
                    {
                        world.setTransform(
                            found->second.body,
                            *relative_center,
                            static_cast<float>(transform.rotation)
                        );
                    }
                }
            );
            if (!success)
            {
                return false;
            }

            for (auto iterator = bodies.begin(); iterator != bodies.end();)
            {
                const auto entity = iterator->first;
                const bool remove = !registry.valid(entity) || !registry.all_of<BoxCollider2D, Transform2D>(entity);
                if (remove)
                {
                    world.destroyBody(iterator->second.body);
                    iterator = bodies.erase(iterator);
                }
                else
                {
                    ++iterator;
                }
            }
            return true;
        }

        [[nodiscard]] bool advance() noexcept
        {
            const auto snapshot = time;
            if (snapshot.delta.count() < 0)
            {
                return false;
            }
            const auto fixed = lux::simulation::SimulationDuration{configuration.fixed_step_nanoseconds};
            const auto maximum = lux::simulation::SimulationDuration{
                configuration.fixed_step_nanoseconds * static_cast<std::int64_t>(configuration.max_substeps)
            };
            accumulator = snapshot.delta >= maximum - accumulator ? maximum : accumulator + snapshot.delta;
            const auto seconds = std::chrono::duration<float>(fixed).count();
            std::uint32_t steps{};
            while (accumulator >= fixed && steps < configuration.max_substeps)
            {
                world.step(seconds);
                accumulator -= fixed;
                ++steps;
                ++completed_steps;
            }
            return true;
        }

        void scatterDynamicBodies() noexcept
        {
            for (const auto& [entity, record] : bodies)
            {
                const bool is_inactive_body =
                    !record.dynamic || !registry.valid(entity) || !registry.all_of<RigidBody2D>(entity);
                if (is_inactive_body)
                {
                    continue;
                }
                const auto position = world.position(record.body).cast<double>() + *origin;
                const auto rotation = static_cast<double>(world.angle(record.body));
                const auto velocity = world.linearVelocity(record.body).cast<double>();
                registry.patch<lux::simulation::ecs::Transform2D>(
                    entity,
                    [&](auto& transform)
                    {
                        transform.translation = position;
                        transform.rotation = rotation;
                    }
                );
                registry.patch<RigidBody2D>(entity, [&](auto& body) { body.velocity = velocity; });
            }
        }
    };

    Physics2DSystem::CreateResult Physics2DSystem::create(
        lux::simulation::ecs::Registry& registry,
        const lux::simulation::SimulationTime& time,
        Physics2DSystemConfiguration configuration
    ) noexcept
    {
        if (!validConfiguration(configuration))
        {
            return lux::cxx::unexpected(EPhysics2DSystemError::INVALID_CONFIGURATION);
        }
        auto world = detail::Box2DWorld::create(
            configuration.gravity_x,
            configuration.gravity_y,
            static_cast<std::size_t>(configuration.body_capacity)
        );
        if (!world)
        {
            return lux::cxx::unexpected(EPhysics2DSystemError::CAPACITY_EXCEEDED);
        }
        auto impl = std::make_unique<Impl>(registry, time, configuration, std::move(*world));
        impl->bodies.reserve(static_cast<std::size_t>(configuration.body_capacity));
        return std::unique_ptr<Physics2DSystem>(new Physics2DSystem(std::move(impl)));
    }

    Physics2DSystem::Physics2DSystem(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    Physics2DSystem::~Physics2DSystem() noexcept = default;

    bool Physics2DSystem::update() noexcept
    {
        if (!impl_->syncBodies() || !impl_->advance())
        {
            return false;
        }
        if (impl_->origin)
        {
            impl_->scatterDynamicBodies();
        }
        return true;
    }

    bool Physics2DSystem::overlapsBox(double center_x, double center_y, double half_width, double half_height) noexcept
    {
        ++impl_->overlap_queries;
        const bool is_invalid_extent =
            !std::isfinite(half_width) || !std::isfinite(half_height) || half_width <= 0.0 || half_height <= 0.0;
        if (is_invalid_extent)
        {
            return false;
        }
        const auto center = impl_->relative({center_x, center_y});
        const Eigen::Vector2d half{half_width, half_height};
        const bool is_invalid_query = !center || !half.allFinite() || half.maxCoeff() > std::numeric_limits<float>::max();
        if (is_invalid_query)
        {
            return false;
        }
        return impl_->world.overlapsBox(*center, half.cast<float>());
    }

    Physics2DRuntimeStats Physics2DSystem::stats() const noexcept
    {
        return {impl_->bodies.size(), impl_->completed_steps, impl_->overlap_queries};
    }

    const lux::simulation::SimulationSystemDescription& physics2DSystemDescription() noexcept
    {
        return Physics2DSystem::Description;
    }

    std::span<const lux::simulation::SimulationSystemRegistration> physics2DSystemRegistrations() noexcept
    {
        static const std::array registrations{lux::simulation::SimulationSystemRegistration{
            .type = lux::system::systemTypeId(Physics2DSystem::Description.type.canonical_name),
            .cpp_type = lux::cxx::typeToken<Physics2DSystem>(),
            .description = &Physics2DSystem::Description,
            .access = Physics2DSystem::Access.spec(),
            .configuration = lux::serialization::makePortableValueCodec<Physics2DSystemConfiguration>(),
            .install = &installPhysics2DSystem
        }};
        return registrations;
    }

    lux::cxx::expected<std::vector<std::byte>, EPhysics2DSystemError> makePhysics2DSystemConfiguration(
        const Physics2DSystemConfiguration& configuration
    ) noexcept
    {
        if (!validConfiguration(configuration))
        {
            return lux::cxx::unexpected(EPhysics2DSystemError::INVALID_CONFIGURATION);
        }
        std::vector<std::byte> result;
        const auto encoded = lux::serialization::makePortableValueCodec<Physics2DSystemConfiguration>().encode(
            std::addressof(configuration),
            result
        );
        if (!encoded)
        {
            return lux::cxx::unexpected(EPhysics2DSystemError::INVALID_CONFIGURATION);
        }
        return result;
    }
} // namespace lux::physics2d
