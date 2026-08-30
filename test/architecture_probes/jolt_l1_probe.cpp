#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

#include <Jolt/Jolt.h>

#ifndef JPH_DOUBLE_PRECISION
#error "Lux Physics3D requires double-position Jolt"
#endif

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

static_assert(std::same_as<JPH::Real, double>);
static_assert(std::same_as<JPH::RVec3, JPH::DVec3>);

namespace
{
    using namespace lux::simulation;

    namespace ObjectLayers
    {
        inline constexpr JPH::ObjectLayer STATIC = 0U;
        inline constexpr JPH::ObjectLayer DYNAMIC = 1U;
        inline constexpr JPH::ObjectLayer COUNT = 2U;
    }

    namespace BroadPhaseLayers
    {
        inline constexpr JPH::BroadPhaseLayer STATIC{0U};
        inline constexpr JPH::BroadPhaseLayer DYNAMIC{1U};
        inline constexpr JPH::uint COUNT = 2U;
    }

    struct AllocationCounters final
    {
        std::atomic_size_t calls{};
        std::atomic_size_t requested_bytes{};
    };

    AllocationCounters allocations;

    void* joltAllocate(std::size_t size) noexcept
    {
        void* memory = std::malloc(size);
        if (memory != nullptr)
        {
            allocations.calls.fetch_add(1U, std::memory_order_relaxed);
            allocations.requested_bytes.fetch_add(size, std::memory_order_relaxed);
        }
        return memory;
    }

    void* joltReallocate(void* memory, std::size_t, std::size_t new_size) noexcept
    {
        void* replacement = std::realloc(memory, new_size);
        if (replacement != nullptr)
        {
            allocations.calls.fetch_add(1U, std::memory_order_relaxed);
            allocations.requested_bytes.fetch_add(new_size, std::memory_order_relaxed);
        }
        return replacement;
    }

    void joltFree(void* memory) noexcept
    {
        std::free(memory);
    }

    void* joltAlignedAllocate(std::size_t size, std::size_t alignment) noexcept
    {
#if defined(_WIN32)
        void* memory = _aligned_malloc(size, alignment);
#else
        void* memory{};
        if (posix_memalign(&memory, alignment, size) != 0)
            memory = nullptr;
#endif
        if (memory != nullptr)
        {
            allocations.calls.fetch_add(1U, std::memory_order_relaxed);
            allocations.requested_bytes.fetch_add(size, std::memory_order_relaxed);
        }
        return memory;
    }

    void joltAlignedFree(void* memory) noexcept
    {
#if defined(_WIN32)
        _aligned_free(memory);
#else
        std::free(memory);
#endif
    }

    class JoltRuntime final
    {
    public:
        JoltRuntime()
        {
            JPH::Allocate = &joltAllocate;
            JPH::Reallocate = &joltReallocate;
            JPH::Free = &joltFree;
            JPH::AlignedAllocate = &joltAlignedAllocate;
            JPH::AlignedFree = &joltAlignedFree;
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }

        ~JoltRuntime() noexcept
        {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }

        JoltRuntime(const JoltRuntime&) = delete;
        JoltRuntime& operator=(const JoltRuntime&) = delete;
    };

    class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface
    {
    public:
        BroadPhaseLayerInterface() noexcept
        {
            layers_[ObjectLayers::STATIC] = BroadPhaseLayers::STATIC;
            layers_[ObjectLayers::DYNAMIC] = BroadPhaseLayers::DYNAMIC;
        }

        [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override
        {
            return BroadPhaseLayers::COUNT;
        }

        [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
        {
            return layers_[layer];
        }

    private:
        JPH::BroadPhaseLayer layers_[ObjectLayers::COUNT];
    };

    class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
    {
    public:
        [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override
        {
            if (first == ObjectLayers::STATIC)
                return second == ObjectLayers::DYNAMIC;
            return first == ObjectLayers::DYNAMIC;
        }
    };

    class ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
    {
    public:
        [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer object, JPH::BroadPhaseLayer broad_phase) const override
        {
            if (object == ObjectLayers::STATIC)
                return broad_phase == BroadPhaseLayers::DYNAMIC;
            return object == ObjectLayers::DYNAMIC;
        }
    };

    class CountingContactListener final : public JPH::ContactListener
    {
    public:
        void OnContactAdded(
            const JPH::Body&,
            const JPH::Body&,
            const JPH::ContactManifold&,
            JPH::ContactSettings&
        ) override
        {
            added_.fetch_add(1U, std::memory_order_relaxed);
        }

        void OnContactPersisted(
            const JPH::Body&,
            const JPH::Body&,
            const JPH::ContactManifold&,
            JPH::ContactSettings&
        ) override
        {
            persisted_.fetch_add(1U, std::memory_order_relaxed);
        }

        [[nodiscard]] std::size_t count() const noexcept
        {
            return added_.load(std::memory_order_relaxed) + persisted_.load(std::memory_order_relaxed);
        }

    private:
        std::atomic_size_t added_{};
        std::atomic_size_t persisted_{};
    };

    struct ProbeConfig final
    {
        double base_x{};
        std::uint32_t dynamic_bodies{};
        std::uint32_t static_bodies{};
        std::uint32_t worker_threads{};
    };

    struct ProbeResult final
    {
        bool ran{};
        bool moved_roundtrip{};
        bool contact{};
        bool raycast{};
        double dynamic_relative_x{};
        double ray_absolute_x{};
        double ray_relative_x{};
        double step_milliseconds{};
        std::uint32_t body_count{};
        std::uint32_t active_bodies{};
        std::uint32_t broadphase_bodies{};
        std::uint32_t worker_threads{};
        std::size_t contact_count{};
        std::size_t body_memory_bytes{};
        std::size_t allocation_calls{};
        std::size_t allocated_bytes{};
    };

    struct ProbeContext final
    {
        ProbeConfig config;
        ProbeResult result;
    };

    class JoltProbeSystem final
    {
    public:
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.test.physics3d.jolt-l1-probe",
            .version = 1U
        };

        explicit JoltProbeSystem(ProbeContext& context)
            : context_(&context),
              worker_threads_(context.config.worker_threads),
              allocation_calls_start_(allocations.calls.load(std::memory_order_relaxed)),
              allocated_bytes_start_(allocations.requested_bytes.load(std::memory_order_relaxed)),
              temp_allocator_(temporaryBytes(context.config)),
              job_system_(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, static_cast<int>(worker_threads_))
        {
            const std::uint32_t max_bodies = context.config.dynamic_bodies + context.config.static_bodies;
            const std::uint32_t max_pairs = std::max(1024U, context.config.dynamic_bodies * 24U + 1024U);
            const std::uint32_t max_contacts = std::max(1024U, context.config.dynamic_bodies * 8U + 1024U);
            physics_.Init(
                max_bodies,
                0U,
                max_pairs,
                max_contacts,
                broad_phase_layers_,
                object_vs_broad_phase_,
                object_pairs_
            );
            physics_.SetGravity(JPH::Vec3::sZero());
            physics_.SetContactListener(&contacts_);
            populate();
        }

        ~JoltProbeSystem() noexcept
        {
            auto& bodies = physics_.GetBodyInterface();
            for (const JPH::BodyID body : body_ids_)
            {
                bodies.RemoveBody(body);
                bodies.DestroyBody(body);
            }
        }

        [[nodiscard]] bool run() noexcept
        {
            constexpr float FixedStep = 1.0F / 60.0F;
            const auto started = std::chrono::steady_clock::now();
            const JPH::EPhysicsUpdateError error = physics_.Update(FixedStep, 1, &temp_allocator_, &job_system_);
            const auto finished = std::chrono::steady_clock::now();
            if (error != JPH::EPhysicsUpdateError::None)
            {
                std::cerr << "jolt_update_error=" << static_cast<std::uint32_t>(error) << '\n';
                return false;
            }

            auto& bodies = physics_.GetBodyInterface();
            const JPH::RVec3 dynamic_position = bodies.GetPosition(dynamic_body_);
            const JPH::RVec3 query_base(context_->config.base_x - 2.0, 0.0, 0.0);
            const JPH::RRayCast ray(query_base, JPH::Vec3(4.0F, 0.0F, 0.0F));
            JPH::RayCastResult ray_hit;
            const bool raycast = physics_.GetNarrowPhaseQuery().CastRay(ray, ray_hit);
            const JPH::RVec3 hit_position = raycast ? ray.GetPointOnRay(ray_hit.mFraction) : JPH::RVec3::sZero();
            const JPH::DVec3 hit_delta = hit_position - JPH::RVec3(context_->config.base_x, 0.0, 0.0);
            const JPH::Vec3 hit_relative{
                static_cast<float>(hit_delta.GetX()),
                static_cast<float>(hit_delta.GetY()),
                static_cast<float>(hit_delta.GetZ())
            };
            const Eigen::Vector3d query_base_eigen{context_->config.base_x, 0.0, 0.0};
            const Eigen::Vector3d reconstructed = query_base_eigen + Eigen::Vector3d{
                static_cast<double>(hit_relative.GetX()),
                static_cast<double>(hit_relative.GetY()),
                static_cast<double>(hit_relative.GetZ())
            };

            ProbeResult& result = context_->result;
            result.ran = true;
            result.contact = physics_.WereBodiesInContact(static_body_, dynamic_body_);
            result.raycast = raycast;
            result.dynamic_relative_x = dynamic_position.GetX() - context_->config.base_x;
            result.ray_absolute_x = reconstructed.x();
            result.ray_relative_x = static_cast<double>(hit_relative.GetX());
            result.step_milliseconds = std::chrono::duration<double, std::milli>(finished - started).count();
            result.body_count = physics_.GetNumBodies();
            result.active_bodies = physics_.GetNumActiveBodies(JPH::EBodyType::RigidBody);
            result.broadphase_bodies = result.body_count;
            result.worker_threads = worker_threads_;
            result.contact_count = contacts_.count();
            result.body_memory_bytes = static_cast<std::size_t>(result.body_count) * sizeof(JPH::Body);
            result.allocation_calls = allocations.calls.load(std::memory_order_relaxed) - allocation_calls_start_;
            result.allocated_bytes =
                allocations.requested_bytes.load(std::memory_order_relaxed) - allocated_bytes_start_;
            return result.moved_roundtrip && result.contact && result.raycast;
        }

    private:
        [[nodiscard]] static std::size_t temporaryBytes(const ProbeConfig& config) noexcept
        {
            constexpr std::size_t Minimum = 16U * 1024U * 1024U;
            const std::size_t per_body =
                static_cast<std::size_t>(config.dynamic_bodies + config.static_bodies) * 2048U;
            return std::max(Minimum, per_body);
        }

        void addBody(const JPH::BodyCreationSettings& settings, JPH::EActivation activation)
        {
            const JPH::BodyID body = physics_.GetBodyInterface().CreateAndAddBody(settings, activation);
            if (body.IsInvalid())
                throw std::runtime_error("Jolt body capacity exhausted");
            body_ids_.push_back(body);
        }

        void populate()
        {
            const ProbeConfig config = context_->config;
            if (config.dynamic_bodies == 0U || config.static_bodies == 0U)
                throw std::runtime_error("Jolt probe requires static and dynamic bodies");

            JPH::RefConst<JPH::Shape> static_shape = new JPH::BoxShape(JPH::Vec3(0.25F, 0.25F, 0.25F));
            JPH::RefConst<JPH::Shape> dynamic_shape = new JPH::SphereShape(0.25F);
            JPH::BodyCreationSettings static_settings(
                static_shape,
                JPH::RVec3(config.base_x, 0.0, 0.0),
                JPH::Quat::sIdentity(),
                JPH::EMotionType::Static,
                ObjectLayers::STATIC
            );
            addBody(static_settings, JPH::EActivation::DontActivate);
            static_body_ = body_ids_.back();

            JPH::BodyCreationSettings dynamic_settings(
                dynamic_shape,
                JPH::RVec3(config.base_x + 0.125, 0.0, 0.0),
                JPH::Quat::sRotation(JPH::Vec3::sAxisY(), 0.125F),
                JPH::EMotionType::Dynamic,
                ObjectLayers::DYNAMIC
            );
            addBody(dynamic_settings, JPH::EActivation::Activate);
            dynamic_body_ = body_ids_.back();

            auto& bodies = physics_.GetBodyInterface();
            const double created_offset = bodies.GetPosition(dynamic_body_).GetX() - config.base_x;
            bodies.SetPosition(
                dynamic_body_,
                JPH::RVec3(config.base_x + 0.25, 0.0, 0.0),
                JPH::EActivation::Activate
            );
            const double moved_offset = bodies.GetPosition(dynamic_body_).GetX() - config.base_x;
            bodies.SetPosition(
                dynamic_body_,
                JPH::RVec3(config.base_x + 0.125, 0.0, 0.0),
                JPH::EActivation::Activate
            );
            bodies.SetLinearVelocity(dynamic_body_, JPH::Vec3(0.0F, 0.0F, 0.0F));
            const double restored_offset = bodies.GetPosition(dynamic_body_).GetX() - config.base_x;
            context_->result.moved_roundtrip = std::abs(created_offset - 0.125) < 1.0e-9 &&
                std::abs(moved_offset - 0.25) < 1.0e-9 &&
                std::abs(restored_offset - 0.125) < 1.0e-9;

            body_ids_.reserve(static_cast<std::size_t>(config.static_bodies) + config.dynamic_bodies);
            const float base_as_float = static_cast<float>(config.base_x);
            const float next_float = std::nextafter(base_as_float, std::numeric_limits<float>::infinity());
            const double broadphase_spacing = std::max(1.25, static_cast<double>(next_float - base_as_float) * 4.0);
            for (std::uint32_t index = 1U; index < config.static_bodies; ++index)
            {
                const double x = config.base_x + broadphase_spacing * (2.0 + static_cast<double>(index % 500U));
                const double z = static_cast<double>(index / 500U) * 1.25;
                static_settings.mPosition = JPH::RVec3(x, 0.0, z);
                addBody(static_settings, JPH::EActivation::DontActivate);
            }
            for (std::uint32_t index = 1U; index < config.dynamic_bodies; ++index)
            {
                const double x = config.base_x + broadphase_spacing * (2.0 + static_cast<double>(index % 500U));
                const double z = static_cast<double>(index / 500U) * 1.25;
                dynamic_settings.mPosition = JPH::RVec3(x + 0.125, 0.0, z);
                addBody(dynamic_settings, JPH::EActivation::Activate);
            }
            physics_.OptimizeBroadPhase();
        }

        ProbeContext* context_{};
        std::uint32_t worker_threads_{};
        std::size_t allocation_calls_start_{};
        std::size_t allocated_bytes_start_{};
        BroadPhaseLayerInterface broad_phase_layers_;
        ObjectVsBroadPhaseLayerFilter object_vs_broad_phase_;
        ObjectLayerPairFilter object_pairs_;
        CountingContactListener contacts_;
        JPH::TempAllocatorImpl temp_allocator_;
        JPH::JobSystemThreadPool job_system_;
        JPH::PhysicsSystem physics_;
        std::vector<JPH::BodyID> body_ids_;
        JPH::BodyID static_body_;
        JPH::BodyID dynamic_body_;
    };

    lux::cxx::expected<void, SystemBuildFailure> installJoltProbe(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto& context = builder.registry().ctx().get<ProbeContext>();
        auto system = builder.emplaceSystem<JoltProbeSystem>(description.instanceId(), context);
        if (!system)
            return lux::cxx::unexpected(system.error());
        return builder.addSystemTask<JoltProbeSystem>(
            description.instanceId(),
            [](JoltProbeSystem& value) noexcept { return value.run(); }
        );
    }

    [[nodiscard]] std::shared_ptr<const SimulationDescription> makeDescription()
    {
        constexpr SystemInstanceId Instance{1U};
        SimulationDescriptionBuilder builder;
        if (!builder.addSystem(Instance, JoltProbeSystem::Description.canonical_name, JoltProbeSystem::Description))
            throw std::runtime_error("Jolt probe description rejected the concrete System");
        auto description = std::move(builder).build();
        if (!description)
            throw std::runtime_error("Jolt probe description build failed");
        return std::make_shared<SimulationDescription>(std::move(*description));
    }

    [[nodiscard]] ProbeResult runScenario(const ProbeConfig& config, const SystemRegistry& system_types)
    {
        lux::simulation::ecs::Registry registry;
        registry.ctx().emplace<ProbeContext>(ProbeContext{config, {}});
        auto simulation = Simulation::create(registry, makeDescription(), system_types);
        if (!simulation)
            throw std::runtime_error("Jolt probe Simulation installation failed");
        auto executor = lux::task::TaskExecutor::create({0U, 1U});
        if (!executor)
            throw std::runtime_error("Jolt probe TaskExecutor creation failed");
        const auto executed = simulation->execute(*executor);
        if (!executed)
            throw std::runtime_error("Jolt probe primary task failed");
        return registry.ctx().get<ProbeContext>().result;
    }

    void printResult(std::string_view name, const ProbeResult& result)
    {
        std::cout << std::setprecision(9)
                  << "scenario=" << name
                  << ",step_ms=" << result.step_milliseconds
                  << ",bodies=" << result.body_count
                  << ",active=" << result.active_bodies
                  << ",broadphase_bodies=" << result.broadphase_bodies
                  << ",contacts=" << result.contact_count
                  << ",body_bytes=" << result.body_memory_bytes
                  << ",allocations=" << result.allocation_calls
                  << ",allocated_bytes=" << result.allocated_bytes
                  << ",workers=" << result.worker_threads
                  << ",dynamic_relative_x=" << result.dynamic_relative_x
                  << ",ray_relative_x=" << result.ray_relative_x
                  << '\n';
    }
}

int main(int argc, char** argv)
{
    try
    {
        const bool qualification = argc == 2 && std::string_view(argv[1]) == "--qualification";
        const std::uint32_t dynamic_bodies = qualification ? 10'000U : 1U;
        const std::uint32_t static_bodies = qualification ? 100'000U : 1U;
        const std::uint32_t available_workers = std::thread::hardware_concurrency() > 1U
            ? std::thread::hardware_concurrency() - 1U
            : 0U;
        const std::uint32_t worker_threads = std::min(3U, available_workers);

        JoltRuntime jolt;
        SystemRegistry system_types;
        const SystemRegistration registration{
            systemTypeId(JoltProbeSystem::Description.canonical_name),
            JoltProbeSystem::Description.version,
            &installJoltProbe
        };
        if (!system_types.add(registration))
            return 2;

        const ProbeResult near_result = runScenario(
            ProbeConfig{0.0, dynamic_bodies, static_bodies, worker_threads},
            system_types
        );
        const ProbeResult far_result = runScenario(
            ProbeConfig{1.0e12, dynamic_bodies, static_bodies, worker_threads},
            system_types
        );
        const bool ran = near_result.ran && far_result.ran;
        const bool roundtrip = near_result.moved_roundtrip && far_result.moved_roundtrip;
        const bool contacts = near_result.contact && far_result.contact;
        const bool rays = near_result.raycast && far_result.raycast;
        const bool same_position =
            std::abs(near_result.dynamic_relative_x - far_result.dynamic_relative_x) < 2.0e-4;
        const bool same_ray = std::abs(near_result.ray_relative_x - far_result.ray_relative_x) < 1.0e-5;
        const bool reconstructed =
            std::abs(far_result.ray_absolute_x - (1.0e12 + far_result.ray_relative_x)) < 1.0e-4;
        printResult("near", near_result);
        printResult("far", far_result);
        if (!ran || !roundtrip || !contacts || !rays || !same_position || !same_ray || !reconstructed)
        {
            std::cerr << "ran=" << ran << ",roundtrip=" << roundtrip << ",contacts=" << contacts
                      << ",rays=" << rays << ",same_position=" << same_position << ",same_ray=" << same_ray
                      << ",reconstructed=" << reconstructed << '\n';
            return 3;
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "jolt_probe_failure=" << error.what() << '\n';
        return 4;
    }
}
