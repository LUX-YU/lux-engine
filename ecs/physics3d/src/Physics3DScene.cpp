#include <lux/engine/ecs/physics3d/systems/Physics3DScene.hpp>

#include <lux/engine/ecs/SpatialTransformMath.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/physics3d/components/Physics3DComponents.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <oneapi/tbb/info.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        constexpr JPH::ObjectLayer kMovingLayerBit = 0x8000u;
        constexpr JPH::ObjectLayer kLogicalLayerMask = 0x000fu;
        constexpr JPH::ObjectLayer kDefaultStaticLayer = 0u;
        constexpr JPH::ObjectLayer kDefaultMovingLayer = kMovingLayerBit;
        constexpr JPH::BroadPhaseLayer kStaticBroadPhase{0u};
        constexpr JPH::BroadPhaseLayer kMovingBroadPhase{1u};
        constexpr float kMaximumPhysicsExtent = 8'388'608.0f;
        constexpr double kOriginRebaseDistance = 8.0 * 1024.0;

        [[nodiscard]] constexpr bool movingLayer(
            JPH::ObjectLayer layer) noexcept
        {
            return (layer & kMovingLayerBit) != 0u;
        }

        [[nodiscard]] constexpr std::uint16_t logicalLayerBit(
            JPH::ObjectLayer layer) noexcept
        {
            return static_cast<std::uint16_t>(
                1u << (layer & kLogicalLayerMask));
        }

        [[nodiscard]] std::optional<JPH::ObjectLayer> makeObjectLayer(
            bool moving,
            std::uint16_t layer) noexcept
        {
            if (layer == 0u || !std::has_single_bit(layer))
            {
                return std::nullopt;
            }
            const auto index = std::countr_zero(layer);
            if (index >= 16)
            {
                return std::nullopt;
            }
            return static_cast<JPH::ObjectLayer>(
                (moving ? kMovingLayerBit : 0u) | index);
        }

        struct JoltGlobalLifetime final
        {
            JoltGlobalLifetime()
            {
                JPH::RegisterDefaultAllocator();
                if (JPH::Factory::sInstance == nullptr)
                {
                    JPH::Factory::sInstance = new JPH::Factory();
                    owns_factory = true;
                }
                JPH::RegisterTypes();
            }

            ~JoltGlobalLifetime()
            {
                JPH::UnregisterTypes();
                if (owns_factory)
                {
                    delete JPH::Factory::sInstance;
                    JPH::Factory::sInstance = nullptr;
                }
            }

            bool owns_factory{false};
        };

        [[nodiscard]] std::shared_ptr<JoltGlobalLifetime>
        acquireJoltLifetime()
        {
            // Jolt's factory/type registry is process-global. Keeping one
            // process-lifetime strong owner avoids the weak-owner ABA where a
            // new scene could register types while the last old shape was
            // concurrently unregistering them.
            static const auto lifetime =
                std::make_shared<JoltGlobalLifetime>();
            return lifetime;
        }

        class TbbJoltJobSystem final : public JPH::JobSystemWithBarrier
        {
        public:
            TbbJoltJobSystem()
                : JobSystemWithBarrier(16u)
                , arena_(std::max(1, oneapi::tbb::info::default_concurrency()))
            {}

            ~TbbJoltJobSystem() noexcept override
            {
                settle();
            }

            [[nodiscard]] int GetMaxConcurrency() const override
            {
                return arena_.max_concurrency();
            }

            void settle() noexcept
            {
                arena_.execute([this]() noexcept
                {
                    tasks_.wait();
                });
            }

            JobHandle CreateJob(
                const char* name,
                JPH::ColorArg color,
                const JobFunction& function,
                JPH::uint32 dependencies) override
            {
                auto* const job = new Job(
                    name, color, this, function, dependencies);
                JobHandle handle{job};
                if (dependencies == 0u)
                {
                    QueueJob(job);
                }
                return handle;
            }

        protected:
            void QueueJob(Job* job) override
            {
                job->AddRef();
                arena_.execute([this, job]() noexcept
                {
                    tasks_.run([job]() noexcept
                    {
                        (void)job->Execute();
                        job->Release();
                    });
                });
            }

            void QueueJobs(Job** jobs, JPH::uint count) override
            {
                for (JPH::uint index = 0u; index < count; ++index)
                {
                    QueueJob(jobs[index]);
                }
            }

            void FreeJob(Job* job) override
            {
                delete job;
            }

        private:
            oneapi::tbb::task_arena arena_;
            oneapi::tbb::task_group tasks_;
        };

        class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
        {
        public:
            JPH::uint GetNumBroadPhaseLayers() const override { return 2u; }

            JPH::BroadPhaseLayer GetBroadPhaseLayer(
                JPH::ObjectLayer layer) const override
            {
                return movingLayer(layer) ?
                    kMovingBroadPhase : kStaticBroadPhase;
            }
        };

        class ObjectVsBroadPhase final :
            public JPH::ObjectVsBroadPhaseLayerFilter
        {
        public:
            bool ShouldCollide(
                JPH::ObjectLayer layer,
                JPH::BroadPhaseLayer broad) const override
            {
                if (!movingLayer(layer))
                {
                    return broad == kMovingBroadPhase;
                }
                return true;
            }
        };

        class ObjectPairs final : public JPH::ObjectLayerPairFilter
        {
        public:
            ObjectPairs()
            {
                masks_.fill(0xffffu);
                assigned_.fill(false);
            }

            [[nodiscard]] bool registerFilter(
                std::uint16_t layer,
                std::uint16_t mask) noexcept
            {
                if (layer == 0u || !std::has_single_bit(layer))
                {
                    return false;
                }
                const auto index = std::countr_zero(layer);
                if (index >= masks_.size())
                {
                    return false;
                }
                if (assigned_[index])
                {
                    return masks_[index] == mask;
                }
                assigned_[index] = true;
                masks_[index] = mask;
                return true;
            }

            bool ShouldCollide(
                JPH::ObjectLayer first,
                JPH::ObjectLayer second) const override
            {
                if (!movingLayer(first) && !movingLayer(second))
                {
                    return false;
                }
                const auto first_index = first & kLogicalLayerMask;
                const auto second_index = second & kLogicalLayerMask;
                return (masks_[first_index] & logicalLayerBit(second)) != 0u &&
                    (masks_[second_index] & logicalLayerBit(first)) != 0u;
            }

        private:
            std::array<std::uint16_t, 16u> masks_{};
            std::array<bool, 16u> assigned_{};
        };

        [[nodiscard]] JPH::Vec3 joltVector(const Eigen::Vector3f& value)
        {
            return {value.x(), value.y(), value.z()};
        }

        [[nodiscard]] Eigen::Vector3f eigenVector(JPH::Vec3Arg value)
        {
            return {value.GetX(), value.GetY(), value.GetZ()};
        }

        [[nodiscard]] JPH::Quat joltQuaternion(
            const Eigen::Quaternionf& value)
        {
            const auto normalized = value.normalized();
            return {
                normalized.x(), normalized.y(), normalized.z(), normalized.w()};
        }

        [[nodiscard]] Eigen::Quaternionf eigenQuaternion(JPH::QuatArg value)
        {
            return Eigen::Quaternionf{
                value.GetW(), value.GetX(), value.GetY(), value.GetZ()}
                .normalized();
        }

        [[nodiscard]] bool finitePositive(float value) noexcept
        {
            return std::isfinite(value) && value > 0.0f;
        }

        [[nodiscard]] lux::cxx::expected<void, std::string>
        validateStaticHeightfieldBatch(
            const StaticHeightfieldBatch3D& batch) noexcept
        {
            if (batch.heightfields.empty())
            {
                return lux::cxx::unexpected(
                    std::string{"static heightfield batch is empty"});
            }
            for (const auto& heightfield : batch.heightfields)
            {
                if (!lux::math::isFinite(heightfield.origin))
                {
                    return lux::cxx::unexpected(std::string{
                        "static heightfield origin is not finite"});
                }
                if (heightfield.sample_edge < 2u)
                {
                    return lux::cxx::unexpected(std::string{
                        "static heightfield sample edge is invalid"});
                }
                const auto edge = static_cast<std::size_t>(
                    heightfield.sample_edge);
                if (edge > std::numeric_limits<std::size_t>::max() / edge ||
                    heightfield.samples.size() != edge * edge)
                {
                    return lux::cxx::unexpected(std::string{
                        "static heightfield sample count is invalid"});
                }
                if (!finitePositive(heightfield.sample_spacing))
                {
                    return lux::cxx::unexpected(std::string{
                        "static heightfield sample spacing is invalid"});
                }
                if (!std::isfinite(heightfield.height_min) ||
                    !std::isfinite(heightfield.height_max) ||
                    !(heightfield.height_max > heightfield.height_min))
                {
                    return lux::cxx::unexpected(std::string{
                        "static heightfield range is invalid"});
                }
                const auto horizontal_extent =
                    static_cast<double>(heightfield.sample_edge - 1u) *
                    static_cast<double>(heightfield.sample_spacing);
                if (!std::isfinite(horizontal_extent) ||
                    horizontal_extent >
                        static_cast<double>(kMaximumPhysicsExtent))
                {
                    return lux::cxx::unexpected(std::string{
                        "static heightfield horizontal extent is invalid"});
                }
                if (!std::isfinite(
                        heightfield.origin.x + horizontal_extent) ||
                    !std::isfinite(
                        heightfield.origin.z + horizontal_extent) ||
                    !std::isfinite(
                        heightfield.origin.y + heightfield.height_min) ||
                    !std::isfinite(
                        heightfield.origin.y + heightfield.height_max))
                {
                    return lux::cxx::unexpected(std::string{
                        "static heightfield bounds are not finite"});
                }
            }
            return {};
        }

        /// Generation-safe lifetime token shared by scene-owned backend
        /// leases.  `owner` is intentionally erased here; only friend member
        /// functions of Physics3DScene cast it back to the private Impl type.
        struct Physics3DSceneControl final
        {
            void* owner{};
            std::uint64_t generation{1u};
        };

    } // namespace

    struct Physics3DPreparedStaticBatch::Impl final
    {
        struct Heightfield final
        {
            JPH::RefConst<JPH::Shape> shape;
            lux::math::Position3d origin;
            lux::math::Position3d near_corner;
            lux::math::Position3d far_corner;
        };

        // Keep Jolt's factory and registered type table alive until every
        // immutable shape reference in fields has been released.
        std::shared_ptr<JoltGlobalLifetime> lifetime;
        std::vector<Heightfield> fields;
    };

    Physics3DPreparedStaticBatch::Physics3DPreparedStaticBatch(
        std::unique_ptr<Impl> impl) noexcept
        : impl_(std::move(impl))
    {}

    Physics3DPreparedStaticBatch::~Physics3DPreparedStaticBatch() noexcept =
        default;
    Physics3DPreparedStaticBatch::Physics3DPreparedStaticBatch(
        Physics3DPreparedStaticBatch&&) noexcept = default;
    Physics3DPreparedStaticBatch&
    Physics3DPreparedStaticBatch::operator=(
        Physics3DPreparedStaticBatch&&) noexcept = default;

    std::uint32_t
    Physics3DPreparedStaticBatch::heightfieldCount() const noexcept
    {
        return impl_ ? static_cast<std::uint32_t>(impl_->fields.size()) : 0u;
    }

    Physics3DExp<std::unique_ptr<Physics3DPreparedStaticBatch>>
    preparePhysics3DStaticBatch(StaticHeightfieldBatch3D batch) noexcept
    {
        if (auto validated = validateStaticHeightfieldBatch(batch);
            !validated)
        {
            return lux::cxx::unexpected(std::move(validated.error()));
        }

        auto impl = std::make_unique<Physics3DPreparedStaticBatch::Impl>();
        impl->lifetime = acquireJoltLifetime();
        impl->fields.reserve(batch.heightfields.size());
        for (auto& heightfield : batch.heightfields)
        {
            std::vector<float> samples(heightfield.samples.size());
            const auto height_scale =
                (heightfield.height_max - heightfield.height_min) / 65535.0f;
            for (std::size_t index = 0u; index < samples.size(); ++index)
            {
                samples[index] = static_cast<float>(
                    heightfield.samples[index]) * height_scale;
            }

            JPH::HeightFieldShapeSettings settings{
                samples.data(),
                JPH::Vec3{0.0f, heightfield.height_min, 0.0f},
                JPH::Vec3{
                    heightfield.sample_spacing,
                    1.0f,
                    heightfield.sample_spacing},
                heightfield.sample_edge};
            auto shape = settings.Create();
            if (shape.HasError())
            {
                return lux::cxx::unexpected(
                    std::string{shape.GetError().c_str()});
            }

            const auto horizontal_extent =
                static_cast<double>(heightfield.sample_edge - 1u) *
                static_cast<double>(heightfield.sample_spacing);
            impl->fields.push_back({
                shape.Get(),
                heightfield.origin,
                lux::math::Position3d{
                    heightfield.origin.x,
                    heightfield.origin.y + heightfield.height_min,
                    heightfield.origin.z},
                lux::math::Position3d{
                    heightfield.origin.x + horizontal_extent,
                    heightfield.origin.y + heightfield.height_max,
                    heightfield.origin.z + horizontal_extent}});
        }
        return std::unique_ptr<Physics3DPreparedStaticBatch>{
            new Physics3DPreparedStaticBatch{std::move(impl)}};
    }

    struct Physics3DScene::Impl final
    {
        struct BodyState final
        {
            JPH::BodyID body;
            lux::math::Position3d position;
            Eigen::Vector3f collider_offset = Eigen::Vector3f::Zero();
            ERigidBody3DMotion motion{ERigidBody3DMotion::DYNAMIC};
            std::optional<lux::math::Position3d> near_corner;
            std::optional<lux::math::Position3d> far_corner;
        };

        struct CharacterState final
        {
            JPH::Ref<JPH::CharacterVirtual> character;
            lux::math::Position3d position;
            JPH::ObjectLayer object_layer{kDefaultMovingLayer};
        };

        class Contacts final : public JPH::ContactListener
        {
        public:
            struct RawFact final
            {
                JPH::BodyID first;
                JPH::BodyID second;
                Eigen::Vector3f normal = Eigen::Vector3f::Zero();
                float penetration{0.0f};
            };

            explicit Contacts(std::size_t maximum_facts)
                : maximum_facts_(maximum_facts)
            {
                facts_.reserve(maximum_facts_);
            }

            void OnContactAdded(
                const JPH::Body& first,
                const JPH::Body& second,
                const JPH::ContactManifold& manifold,
                JPH::ContactSettings&) override
            {
                const std::scoped_lock lock{mutex_};
                if (facts_.size() >= maximum_facts_)
                {
                    ++dropped_;
                    return;
                }
                facts_.push_back({
                    first.GetID(),
                    second.GetID(),
                    eigenVector(manifold.mWorldSpaceNormal),
                    manifold.mPenetrationDepth});
            }

            void take(std::vector<RawFact>& destination)
            {
                const std::scoped_lock lock{mutex_};
                destination.clear();
                destination.swap(facts_);
            }

            [[nodiscard]] std::uint64_t dropped() const noexcept
            {
                const std::scoped_lock lock{mutex_};
                return dropped_;
            }

            [[nodiscard]] std::size_t capacityBytes() const noexcept
            {
                const std::scoped_lock lock{mutex_};
                return facts_.capacity() * sizeof(RawFact);
            }

        private:
            mutable std::mutex mutex_;
            std::vector<RawFact> facts_;
            std::size_t maximum_facts_{0u};
            std::uint64_t dropped_{0u};
        };

        explicit Impl(Physics3DConfig value)
            : config(std::move(value))
            , lifetime(acquireJoltLifetime())
            , control(std::make_shared<Physics3DSceneControl>())
            , scratch(config.temporary_allocator_bytes)
            , contact_listener(config.maximum_contact_constraints)
        {
            control->owner = this;
            physics.Init(
                config.maximum_bodies,
                0u,
                config.maximum_body_pairs,
                config.maximum_contact_constraints,
                broad_phase,
                object_vs_broad_phase,
                object_pairs);
            physics.SetGravity(joltVector(config.gravity));
            physics.SetContactListener(&contact_listener);
            // Static adoption is body-granular. Reserve the two identity
            // tables at scene construction so a later bounded advance cannot
            // hide an O(live bodies) unordered-map rehash.
            body_to_entity.reserve(config.maximum_bodies);
            static_heightfields.reserve(config.maximum_bodies);
            raw_contacts.reserve(config.maximum_contact_constraints);
            contacts.reserve(config.maximum_contact_constraints);
        }

        ~Impl()
        {
            physics.SetContactListener(nullptr);
            for (const auto& [_, body] : dynamic_bodies)
            {
                physics.GetBodyInterface().RemoveBody(body.body);
                physics.GetBodyInterface().DestroyBody(body.body);
            }
            dynamic_bodies.clear();
            characters.clear();
            body_to_entity.clear();
            if (!static_heightfields.empty())
            {
                std::abort();
            }
            control->owner = nullptr;
            ++control->generation;
        }

        [[nodiscard]] std::optional<Eigen::Vector3f> relative(
            const lux::math::Position3d& position) const noexcept
        {
            return relativePosition(
                position, physics_origin, kMaximumPhysicsExtent);
        }

        [[nodiscard]] lux::math::Position3d absolute(
            JPH::RVec3Arg position) const noexcept
        {
            return {
                physics_origin.x + static_cast<double>(position.GetX()),
                physics_origin.y + static_cast<double>(position.GetY()),
                physics_origin.z + static_cast<double>(position.GetZ())};
        }

        [[nodiscard]] bool maybeRebase(
            const lux::math::Position3d& target) noexcept
        {
            if (!lux::math::isFinite(target))
            {
                return false;
            }
            const auto dx = std::abs(target.x - physics_origin.x);
            const auto dy = std::abs(target.y - physics_origin.y);
            const auto dz = std::abs(target.z - physics_origin.z);
            if (std::max({dx, dy, dz}) <= kOriginRebaseDistance)
                return true;

            const auto previous_origin = physics_origin;
            const auto next_origin = target;
            struct RebasedBody final
            {
                JPH::BodyID body;
                JPH::Quat rotation;
                lux::math::Position3d world;
                Eigen::Vector3f relative;
            };
            std::vector<RebasedBody> rebased;
            rebased.reserve(
                dynamic_bodies.size() + static_heightfields.size());
            auto& bodies = physics.GetBodyInterface();
            for (auto& [_, state] : dynamic_bodies)
            {
                JPH::RVec3 old_position;
                JPH::Quat rotation;
                bodies.GetPositionAndRotation(
                    state.body, old_position, rotation);
                const lux::math::Position3d world{
                    previous_origin.x + static_cast<double>(old_position.GetX()),
                    previous_origin.y + static_cast<double>(old_position.GetY()),
                    previous_origin.z + static_cast<double>(old_position.GetZ())};
                const auto relative_position = relativePosition(
                    world,
                    next_origin,
                    kMaximumPhysicsExtent);
                if (!relative_position)
                {
                    return false;
                }
                rebased.push_back({
                    state.body, rotation, world, *relative_position});
            }
            for (auto& [_, state] : static_heightfields)
            {
                JPH::RVec3 old_position;
                JPH::Quat rotation;
                bodies.GetPositionAndRotation(
                    state.body, old_position, rotation);
                const auto relative_position = relativePosition(
                    state.position,
                    next_origin,
                    kMaximumPhysicsExtent);
                if (!relative_position ||
                    (state.near_corner &&
                     !relativePosition(
                         *state.near_corner,
                         next_origin,
                         kMaximumPhysicsExtent)) ||
                    (state.far_corner &&
                     !relativePosition(
                         *state.far_corner,
                         next_origin,
                         kMaximumPhysicsExtent)))
                    return false;
                rebased.push_back({
                    state.body, rotation, state.position, *relative_position});
            }
            std::vector<std::pair<
                JPH::CharacterVirtual*, Eigen::Vector3f>> rebased_characters;
            rebased_characters.reserve(characters.size());
            for (auto& [_, state] : characters)
            {
                const auto old = state.character->GetPosition();
                const lux::math::Position3d world{
                    previous_origin.x + static_cast<double>(old.GetX()),
                    previous_origin.y + static_cast<double>(old.GetY()),
                    previous_origin.z + static_cast<double>(old.GetZ())};
                const auto relative_position = relativePosition(
                    world,
                    next_origin,
                    kMaximumPhysicsExtent);
                if (!relative_position)
                {
                    return false;
                }
                state.position = world;
                rebased_characters.emplace_back(
                    state.character.GetPtr(), *relative_position);
            }
            physics_origin = next_origin;
            for (const auto& item : rebased)
            {
                bodies.SetPositionAndRotation(
                    item.body,
                    JPH::RVec3{
                        item.relative.x(),
                        item.relative.y(),
                        item.relative.z()},
                    item.rotation,
                    JPH::EActivation::DontActivate);
            }
            for (const auto& [character, position] : rebased_characters)
            {
                character->SetPosition(JPH::RVec3{
                    position.x(), position.y(), position.z()});
            }
            for (auto& [_, state] : dynamic_bodies)
            {
                const auto found = std::ranges::find_if(
                    rebased,
                    [&state](const RebasedBody& item)
                    {
                        return item.body == state.body;
                    });
                if (found != rebased.end())
                {
                    state.position = found->world;
                }
            }
            return true;
        }

        [[nodiscard]] bool establishStaticStagingOrigin(
            const lux::math::Position3d& target) noexcept
        {
            if (!lux::math::isFinite(target))
            {
                return false;
            }
            // Static adoption is budgeted by body count. Re-basing every live
            // dynamic/static/character owner here would smuggle unbounded
            // work into beginStaticHeightfieldStaging(). An empty scene can
            // choose its first private origin in O(1); otherwise the incoming
            // batch must already fit the established origin.
            if (dynamic_bodies.empty() && static_heightfields.empty() &&
                characters.empty())
            {
                physics_origin = target;
                return true;
            }
            return relative(target).has_value();
        }

        [[nodiscard]] JPH::RefConst<JPH::Shape> makeShape(
            const Collider3DComponent& collider,
            const Transform3DComponent& transform,
            std::string& error) noexcept
        {
            const auto scale = transform.scale.cwiseAbs();
            JPH::Shape::ShapeResult result;
            switch (collider.shape)
            {
            case ECollider3DShape::BOX:
            {
                const auto half = collider.half_extents.cwiseProduct(scale);
                if (!finitePositive(half.x()) || !finitePositive(half.y()) ||
                    !finitePositive(half.z()))
                {
                    error = "invalid BOX collider dimensions";
                    return {};
                }
                result = JPH::BoxShapeSettings{joltVector(half)}.Create();
                break;
            }
            case ECollider3DShape::SPHERE:
            {
                const auto radius = collider.radius * scale.maxCoeff();
                if (!finitePositive(radius))
                {
                    error = "invalid SPHERE collider radius";
                    return {};
                }
                result = JPH::SphereShapeSettings{radius}.Create();
                break;
            }
            case ECollider3DShape::CAPSULE:
            {
                const auto radius = collider.radius *
                    std::max(scale.x(), scale.z());
                const auto half_height = collider.half_height * scale.y();
                if (!finitePositive(radius) || !finitePositive(half_height))
                {
                    error = "invalid CAPSULE collider dimensions";
                    return {};
                }
                result = JPH::CapsuleShapeSettings{
                    half_height, radius}.Create();
                break;
            }
            }
            if (result.HasError())
            {
                error = result.GetError().c_str();
                return {};
            }
            return result.Get();
        }

        void ensureBodies(lux::meta::EntityRegistry& registry) noexcept
        {
            auto view = registry.view<
                RigidBody3DComponent,
                Collider3DComponent,
                Transform3DComponent>();
            for (const auto entity : view)
            {
                if (dynamic_bodies.contains(entity))
                {
                    continue;
                }
                const auto* hierarchy = registry.try_get<ParentComponent>(
                    entity);
                const auto& transform = view.get<Transform3DComponent>(entity);
                if (hierarchy && hierarchy->parent() != lux::meta::null_entity)
                {
                    continue;
                }

                if (!maybeRebase(transform.position))
                {
                    continue;
                }
                const auto& collider = view.get<Collider3DComponent>(entity);
                std::string shape_error;
                auto shape = makeShape(collider, transform, shape_error);
                if (!shape)
                {
                    continue;
                }
                const auto& rigid = view.get<RigidBody3DComponent>(entity);
                const auto rotation = transform.rotation.normalized();
                const auto pose = makePhysicsRelativePose(
                    transform.position,
                    rotation,
                    physics_origin,
                    kMaximumPhysicsExtent);
                if (!pose)
                {
                    continue;
                }
                const auto center = pose->position +
                    rotation * collider.offset;
                const auto motion = rigid.motion == ERigidBody3DMotion::DYNAMIC ?
                    JPH::EMotionType::Dynamic :
                    rigid.motion == ERigidBody3DMotion::KINEMATIC ?
                        JPH::EMotionType::Kinematic :
                        JPH::EMotionType::Static;
                const auto* collision_filter = registry.try_get<
                    CollisionFilter3DComponent>(entity);
                const auto layer = collision_filter ?
                    collision_filter->layer : std::uint16_t{1u};
                const auto mask = collision_filter ?
                    collision_filter->mask : std::uint16_t{0xffffu};
                const auto object_layer = makeObjectLayer(
                    motion != JPH::EMotionType::Static, layer);
                if (!object_layer || !object_pairs.registerFilter(layer, mask))
                {
                    continue;
                }
                JPH::BodyCreationSettings settings{
                    shape,
                    JPH::RVec3{center.x(), center.y(), center.z()},
                    joltQuaternion(rotation),
                    motion,
                    *object_layer};
                settings.mIsSensor = collider.sensor;
                settings.mGravityFactor = rigid.gravity_factor;
                settings.mMotionQuality = rigid.continuous_collision ?
                    JPH::EMotionQuality::LinearCast :
                    JPH::EMotionQuality::Discrete;
                if (motion == JPH::EMotionType::Dynamic)
                {
                    settings.mOverrideMassProperties =
                        JPH::EOverrideMassProperties::CalculateInertia;
                    settings.mMassPropertiesOverride.mMass =
                        std::max(rigid.mass, 0.001f);
                }
                auto* const body = physics.GetBodyInterface().CreateBody(
                    settings);
                if (!body)
                {
                    continue;
                }
                const auto body_id = body->GetID();
                physics.GetBodyInterface().AddBody(
                    body_id,
                    motion == JPH::EMotionType::Static ?
                        JPH::EActivation::DontActivate :
                        JPH::EActivation::Activate);
                physics.GetBodyInterface().SetLinearAndAngularVelocity(
                    body_id,
                    joltVector(rigid.linear_velocity),
                    joltVector(rigid.angular_velocity));
                BodyState state{
                    body_id,
                    transform.position,
                    collider.offset,
                    rigid.motion};
                const auto [dynamic_iterator, dynamic_inserted] =
                    dynamic_bodies.emplace(entity, state);
                const auto [mapping_iterator, mapping_inserted] =
                    body_to_entity.emplace(
                        body_id.GetIndexAndSequenceNumber(), entity);
                if (!dynamic_inserted || !mapping_inserted)
                {
                    if (dynamic_inserted)
                    {
                        dynamic_bodies.erase(dynamic_iterator);
                    }
                    if (mapping_inserted)
                    {
                        body_to_entity.erase(mapping_iterator);
                    }
                    physics.GetBodyInterface().RemoveBody(body_id);
                    physics.GetBodyInterface().DestroyBody(body_id);
                    continue;
                }
            }
        }

        void pruneBodies(lux::meta::EntityRegistry& registry) noexcept
        {
            auto& bodies = physics.GetBodyInterface();
            for (auto iterator = dynamic_bodies.begin();
                 iterator != dynamic_bodies.end();)
            {
                const auto entity = iterator->first;
                const bool valid = registry.valid(entity) && registry.all_of<
                    RigidBody3DComponent,
                    Collider3DComponent,
                    Transform3DComponent>(entity);
                if (valid)
                {
                    ++iterator;
                    continue;
                }
                bodies.RemoveBody(iterator->second.body);
                bodies.DestroyBody(iterator->second.body);
                body_to_entity.erase(
                    iterator->second.body.GetIndexAndSequenceNumber());
                iterator = dynamic_bodies.erase(iterator);
            }
        }

        void ensureCharacters(lux::meta::EntityRegistry& registry) noexcept
        {
            auto view = registry.view<
                CharacterController3DComponent,
                Collider3DComponent,
                Transform3DComponent>();
            for (const auto entity : view)
            {
                if (characters.contains(entity))
                {
                    continue;
                }
                if (registry.any_of<RigidBody3DComponent>(entity))
                {
                    continue;
                }
                const auto* hierarchy = registry.try_get<ParentComponent>(
                    entity);
                const auto& transform = view.get<Transform3DComponent>(entity);
                if (hierarchy && hierarchy->parent() != lux::meta::null_entity)
                {
                    continue;
                }
                const auto& collider = view.get<Collider3DComponent>(entity);
                if (!maybeRebase(transform.position))
                {
                    continue;
                }
                const auto relative_position = relative(transform.position);
                if (!relative_position)
                {
                    continue;
                }
                std::string shape_error;
                // A CharacterController has capsule semantics regardless of
                // the generic Collider shape selected for rigid bodies. This
                // keeps the public controller contract authorable without
                // leaking Jolt shape enums or requiring an invalid box-shaped
                // character configuration.
                auto character_collider = collider;
                character_collider.shape = ECollider3DShape::CAPSULE;
                auto shape = makeShape(
                    character_collider, transform, shape_error);
                if (!shape)
                {
                    continue;
                }
                const auto& controller =
                    view.get<CharacterController3DComponent>(entity);
                const auto* collision_filter = registry.try_get<
                    CollisionFilter3DComponent>(entity);
                const auto layer = collision_filter ?
                    collision_filter->layer : std::uint16_t{1u};
                const auto mask = collision_filter ?
                    collision_filter->mask : std::uint16_t{0xffffu};
                const auto object_layer = makeObjectLayer(true, layer);
                if (!object_layer || !object_pairs.registerFilter(layer, mask))
                {
                    continue;
                }
                JPH::CharacterVirtualSettings settings;
                settings.mShape = shape;
                settings.mShapeOffset = JPH::Vec3{
                    0.0f,
                    collider.half_height * std::abs(transform.scale.y()) +
                        collider.radius * transform.scale.cwiseAbs().maxCoeff(),
                    0.0f};
                settings.mCharacterPadding = std::max(
                    controller.skin, 0.0001f);
                settings.mMaxSlopeAngle = JPH::DegreesToRadians(
                    std::clamp(
                        controller.maximum_slope_degrees, 0.0f, 89.0f));
                auto character = JPH::Ref<JPH::CharacterVirtual>{
                    new JPH::CharacterVirtual{
                        &settings,
                        JPH::RVec3{
                            relative_position->x(),
                            relative_position->y(),
                            relative_position->z()},
                        joltQuaternion(transform.rotation),
                        static_cast<JPH::uint64>(entt::to_integral(entity)),
                        &physics}};
                characters.emplace(
                    entity,
                    CharacterState{
                        std::move(character),
                        transform.position,
                        *object_layer});
            }
        }

        void pruneCharacters(lux::meta::EntityRegistry& registry) noexcept
        {
            for (auto iterator = characters.begin();
                 iterator != characters.end();)
            {
                const auto entity = iterator->first;
                const bool valid = registry.valid(entity) && registry.all_of<
                    CharacterController3DComponent,
                    Collider3DComponent,
                    Transform3DComponent>(entity) &&
                    !registry.any_of<RigidBody3DComponent>(entity);
                if (valid)
                    ++iterator;
                else
                    iterator = characters.erase(iterator);
            }
        }

        void stepCharacters(
            lux::meta::EntityRegistry& registry,
            float fixed_dt) noexcept
        {
            const JPH::BodyFilter body_filter;
            const JPH::ShapeFilter shape_filter;
            for (auto& [entity, state] : characters)
            {
                if (!registry.valid(entity))
                {
                    continue;
                }
                const auto broad_filter =
                    physics.GetDefaultBroadPhaseLayerFilter(
                        state.object_layer);
                const auto layer_filter = physics.GetDefaultLayerFilter(
                    state.object_layer);
                auto& component = registry.get<
                    CharacterController3DComponent>(entity);
                auto velocity = component.desired_velocity;
                const auto current = state.character->GetLinearVelocity();
                if (!state.character->IsSupported())
                {
                    velocity.y() = current.GetY() + config.gravity.y() * fixed_dt;
                }
                else
                    velocity.y() = std::max(velocity.y(), 0.0f);
                state.character->SetLinearVelocity(joltVector(velocity));
                JPH::CharacterVirtual::ExtendedUpdateSettings settings;
                settings.mWalkStairsStepUp = JPH::Vec3{
                    0.0f, std::max(component.step_height, 0.0f), 0.0f};
                settings.mStickToFloorStepDown = JPH::Vec3{
                    0.0f, -std::max(component.step_height, 0.0f), 0.0f};
                state.character->ExtendedUpdate(
                    fixed_dt,
                    joltVector(config.gravity),
                    settings,
                    broad_filter,
                    layer_filter,
                    body_filter,
                    shape_filter,
                    scratch);
            }
        }

        void syncKinematics(lux::meta::EntityRegistry& registry) noexcept
        {
            auto& bodies = physics.GetBodyInterface();
            for (auto& [entity, state] : dynamic_bodies)
            {
                if (state.motion != ERigidBody3DMotion::KINEMATIC ||
                    !registry.valid(entity))
                    continue;
                const auto& transform = registry.get<Transform3DComponent>(
                    entity);
                const auto relative_position = relative(transform.position);
                if (!relative_position)
                {
                    continue;
                }
                const auto center = *relative_position +
                    transform.rotation * state.collider_offset;
                bodies.SetPositionAndRotation(
                    state.body,
                    JPH::RVec3{center.x(), center.y(), center.z()},
                    joltQuaternion(transform.rotation),
                    JPH::EActivation::Activate);
                state.position = transform.position;
            }
        }

        void scatter(lux::meta::EntityRegistry& registry) noexcept
        {
            auto& bodies = physics.GetBodyInterface();
            for (auto& [entity, state] : dynamic_bodies)
            {
                if (state.motion != ERigidBody3DMotion::DYNAMIC ||
                    !registry.valid(entity))
                    continue;
                JPH::RVec3 position;
                JPH::Quat rotation;
                bodies.GetPositionAndRotation(state.body, position, rotation);
                auto world_position = absolute(position);
                const auto world_rotation = eigenQuaternion(rotation);
                const auto rotated_offset = world_rotation *
                    state.collider_offset;
                world_position.x -= static_cast<double>(rotated_offset.x());
                world_position.y -= static_cast<double>(rotated_offset.y());
                world_position.z -= static_cast<double>(rotated_offset.z());
                state.position = world_position;

                registry.patch<Transform3DComponent>(
                    entity,
                    [&world_position, &world_rotation](
                        Transform3DComponent& transform)
                    {
                        transform.position = world_position;
                        transform.rotation = world_rotation;
                    });

                auto& rigid = registry.get<RigidBody3DComponent>(entity);
                rigid.linear_velocity = eigenVector(
                    bodies.GetLinearVelocity(state.body));
                rigid.angular_velocity = eigenVector(
                    bodies.GetAngularVelocity(state.body));
                registry.patch<RigidBody3DComponent>(entity);
            }
            for (auto& [entity, state] : characters)
            {
                if (!registry.valid(entity))
                {
                    continue;
                }
                const auto position = state.character->GetPosition();
                auto world_position = absolute(position);
                state.position = world_position;
                const auto world_rotation = eigenQuaternion(
                    state.character->GetRotation());
                registry.patch<Transform3DComponent>(
                    entity,
                    [&world_position, &world_rotation](
                        Transform3DComponent& transform)
                    {
                        transform.position = world_position;
                        transform.rotation = world_rotation;
                    });

                auto& controller = registry.get<
                    CharacterController3DComponent>(entity);
                controller.grounded = state.character->IsSupported();
                registry.patch<CharacterController3DComponent>(entity);
            }
        }

        void collectContacts(lux::meta::EntityRegistry& registry)
        {
            contacts.clear();
            const auto valid_owner = [this, &registry](
                                         JPH::BodyID body,
                                         entt::entity entity) noexcept
            {
                if (!registry.valid(entity))
                {
                    return false;
                }
                const auto dynamic = dynamic_bodies.find(entity);
                if (dynamic != dynamic_bodies.end() &&
                    dynamic->second.body == body)
                {
                    return registry.all_of<
                        RigidBody3DComponent,
                        Collider3DComponent,
                        Transform3DComponent>(entity);
                }
                const auto static_body = static_heightfields.find(
                    body.GetIndexAndSequenceNumber());
                return static_body != static_heightfields.end() &&
                    static_body->second.body == body &&
                    registry.all_of<
                        StaticColliderBatch3DComponent,
                        ResolvedTransform3DComponent>(entity);
            };
            contact_listener.take(raw_contacts);
            for (const auto& raw : raw_contacts)
            {
                const auto first = body_to_entity.find(
                    raw.first.GetIndexAndSequenceNumber());
                const auto second = body_to_entity.find(
                    raw.second.GetIndexAndSequenceNumber());
                if (first == body_to_entity.end() ||
                    second == body_to_entity.end())
                    continue;
                const auto first_entity = first->second;
                const auto second_entity = second->second;
                if (!valid_owner(raw.first, first_entity) ||
                    !valid_owner(raw.second, second_entity))
                {
                    continue;
                }
                contacts.push_back({
                    first_entity,
                    second_entity,
                    raw.normal,
                    raw.penetration});
            }
        }

        Physics3DConfig config;
        std::shared_ptr<JoltGlobalLifetime> lifetime;
        std::shared_ptr<Physics3DSceneControl> control;
        BroadPhaseLayers broad_phase;
        ObjectVsBroadPhase object_vs_broad_phase;
        ObjectPairs object_pairs;
        JPH::PhysicsSystem physics;
        JPH::TempAllocatorImpl scratch;
        TbbJoltJobSystem jobs;
        Contacts contact_listener;
        std::vector<Contacts::RawFact> raw_contacts;
        lux::math::Position3d physics_origin;
        float accumulator{0.0f};
        std::unordered_map<entt::entity, BodyState> dynamic_bodies;
        std::unordered_map<entt::entity, CharacterState> characters;
        std::unordered_map<std::uint32_t, entt::entity> body_to_entity;
        std::unordered_map<std::uint32_t, BodyState> static_heightfields;
        std::vector<Physics3DContactFact> contacts;
    };

    struct Physics3DStaticBatchLease::Impl final
    {
        std::weak_ptr<Physics3DSceneControl> control;
        std::uint64_t generation{0u};
        entt::entity owner{entt::null};
        std::vector<JPH::BodyID> bodies;
        std::unique_ptr<Physics3DPreparedStaticBatch> discarded_prepared;
        bool active{false};
        bool retired{false};
    };

    Physics3DStaticBatchLease::Physics3DStaticBatchLease(
        std::unique_ptr<Impl> impl) noexcept
        : impl_(std::move(impl))
    {}

    Physics3DStaticBatchLease::~Physics3DStaticBatchLease() noexcept
    {
        retire();
    }
    Physics3DStaticBatchLease::Physics3DStaticBatchLease(
        Physics3DStaticBatchLease&&) noexcept = default;
    Physics3DStaticBatchLease& Physics3DStaticBatchLease::operator=(
        Physics3DStaticBatchLease&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }
        retire();
        impl_ = std::move(other.impl_);
        return *this;
    }

    void Physics3DStaticBatchLease::activate() noexcept
    {
        if (!impl_ || impl_->active || impl_->retired ||
            impl_->bodies.empty() || impl_->discarded_prepared)
            std::abort();
        const auto control = impl_->control.lock();
        if (!control || control->generation != impl_->generation ||
            !control->owner)
            std::abort();
        auto* const owner = static_cast<Physics3DScene::Impl*>(
            control->owner);
        for (const auto body : impl_->bodies)
        {
            owner->physics.GetBodyInterface().AddBody(
                body, JPH::EActivation::DontActivate);
        }
        impl_->active = true;
    }

    void Physics3DStaticBatchLease::deactivate() noexcept
    {
        if (!impl_ || !impl_->active)
        {
            return;
        }
        const auto control = impl_->control.lock();
        if (!control || control->generation != impl_->generation ||
            !control->owner)
            std::abort();
        auto* const owner = static_cast<Physics3DScene::Impl*>(
            control->owner);
        for (const auto body : impl_->bodies)
        {
            owner->physics.GetBodyInterface().RemoveBody(body);
        }
        impl_->active = false;
    }

    void Physics3DStaticBatchLease::retire() noexcept
    {
        while (!retireSome(std::numeric_limits<std::uint32_t>::max()))
        {}
    }

    bool Physics3DStaticBatchLease::retireSome(
        std::uint32_t maximum_units) noexcept
    {
        if (!impl_ || impl_->retired)
        {
            return true;
        }
        if (maximum_units == 0u)
        {
            return false;
        }
        const auto control = impl_->control.lock();
        if (!control || control->generation != impl_->generation ||
            !control->owner)
        {
            if (impl_->active)
            {
                std::abort();
            }
            std::uint32_t retired = 0u;
            while (!impl_->bodies.empty() && retired < maximum_units)
            {
                impl_->bodies.pop_back();
                ++retired;
            }
            if (impl_->discarded_prepared)
            {
                auto& fields = impl_->discarded_prepared->impl_->fields;
                while (!fields.empty() && retired < maximum_units)
                {
                    fields.pop_back();
                    ++retired;
                }
                if (fields.empty())
                {
                    impl_->discarded_prepared.reset();
                }
            }
            impl_->retired = impl_->bodies.empty() &&
                !impl_->discarded_prepared;
            return impl_->retired;
        }
        auto* const owner = static_cast<Physics3DScene::Impl*>(
            control->owner);
        if (impl_->active)
        {
            deactivate();
        }
        std::uint32_t retired = 0u;
        while (!impl_->bodies.empty() && retired < maximum_units)
        {
            const auto body = impl_->bodies.back();
            impl_->bodies.pop_back();
            owner->physics.GetBodyInterface().DestroyBody(body);
            owner->static_heightfields.erase(
                body.GetIndexAndSequenceNumber());
            owner->body_to_entity.erase(
                body.GetIndexAndSequenceNumber());
            ++retired;
        }
        if (impl_->discarded_prepared)
        {
            auto& fields = impl_->discarded_prepared->impl_->fields;
            while (!fields.empty() && retired < maximum_units)
            {
                fields.pop_back();
                ++retired;
            }
            if (fields.empty())
            {
                impl_->discarded_prepared.reset();
            }
        }
        if (impl_->bodies.empty() && !impl_->discarded_prepared)
        {
            impl_->retired = true;
        }
        return impl_->retired;
    }

    bool Physics3DStaticBatchLease::active() const noexcept
    {
        return impl_ && impl_->active;
    }

    std::uint32_t Physics3DStaticBatchLease::remainingBodies() const noexcept
    {
        return impl_ && !impl_->retired
            ? static_cast<std::uint32_t>(impl_->bodies.size())
            : 0u;
    }

    std::uint32_t
    Physics3DStaticBatchLease::remainingRetirementUnits() const noexcept
    {
        if (!impl_ || impl_->retired)
        {
            return 0u;
        }
        const auto fields = impl_->discarded_prepared
            ? impl_->discarded_prepared->impl_->fields.size()
            : 0u;
        const auto total = impl_->bodies.size() + fields;
        if (total > (std::numeric_limits<std::uint32_t>::max)())
        {
            std::abort();
        }
        return static_cast<std::uint32_t>(total);
    }

    struct Physics3DStaticBatchStager::Impl final
    {
        std::unique_ptr<Physics3DPreparedStaticBatch> prepared;
        std::unique_ptr<Physics3DStaticBatchLease> lease;
        std::string failure;
        bool failed{false};
    };

    Physics3DStaticBatchStager::Physics3DStaticBatchStager(
        std::unique_ptr<Impl> impl) noexcept
        : impl_(std::move(impl))
    {}

    Physics3DStaticBatchStager::~Physics3DStaticBatchStager() noexcept =
        default;
    Physics3DStaticBatchStager::Physics3DStaticBatchStager(
        Physics3DStaticBatchStager&&) noexcept = default;
    Physics3DStaticBatchStager&
    Physics3DStaticBatchStager::operator=(
        Physics3DStaticBatchStager&&) noexcept = default;

    lux::cxx::expected<bool, std::string>
    Physics3DStaticBatchStager::advance(
        std::uint32_t maximum_bodies) noexcept
    {
        if (!impl_ || !impl_->lease || maximum_bodies == 0u ||
            (!impl_->failed && !impl_->prepared))
        {
            return lux::cxx::unexpected(
                std::string{"static heightfield stager is not advanceable"});
        }
        if (impl_->failed)
        {
            if (!impl_->lease->retireSome(maximum_bodies))
            {
                return false;
            }
            return lux::cxx::unexpected(std::move(impl_->failure));
        }
        const auto control = impl_->lease->impl_->control.lock();
        if (!control ||
            control->generation != impl_->lease->impl_->generation ||
            !control->owner)
        {
            impl_->failed = true;
            impl_->failure = "static heightfield scene expired";
            impl_->lease->impl_->discarded_prepared =
                std::move(impl_->prepared);
            return false;
        }
        auto* const owner = static_cast<Physics3DScene::Impl*>(
            control->owner);
        auto& fields = impl_->prepared->impl_->fields;
        std::uint32_t staged = 0u;
        while (!fields.empty() && staged < maximum_bodies)
        {
            const auto& field = fields.back();
            const auto relative_origin = owner->relative(field.origin);
            if (!relative_origin || !owner->relative(field.near_corner) ||
                !owner->relative(field.far_corner))
            {
                impl_->failed = true;
                impl_->failure =
                    "static heightfield batch exceeds PhysicsOrigin";
                impl_->lease->impl_->discarded_prepared =
                    std::move(impl_->prepared);
                return false;
            }
            JPH::BodyCreationSettings settings{
                field.shape,
                JPH::RVec3{
                    relative_origin->x(),
                    relative_origin->y(),
                    relative_origin->z()},
                JPH::Quat::sIdentity(),
                JPH::EMotionType::Static,
                kDefaultStaticLayer};
            auto* const body = owner->physics.GetBodyInterface().CreateBody(
                settings);
            if (!body)
            {
                impl_->failed = true;
                impl_->failure =
                    "Jolt static body capacity exhausted";
                impl_->lease->impl_->discarded_prepared =
                    std::move(impl_->prepared);
                return false;
            }

            Physics3DScene::Impl::BodyState state;
            state.body = body->GetID();
            state.position = field.origin;
            state.motion = ERigidBody3DMotion::STATIC;
            state.near_corner = field.near_corner;
            state.far_corner = field.far_corner;
            const auto body_key = state.body.GetIndexAndSequenceNumber();
            const auto [static_iterator, static_inserted] =
                owner->static_heightfields.emplace(body_key, state);
            if (!static_inserted)
            {
                owner->physics.GetBodyInterface().DestroyBody(state.body);
                impl_->failed = true;
                impl_->failure = "Jolt static body identity collision";
                impl_->lease->impl_->discarded_prepared =
                    std::move(impl_->prepared);
                return false;
            }
            const auto [mapping_iterator, mapping_inserted] =
                owner->body_to_entity.emplace(
                    body_key, impl_->lease->impl_->owner);
            if (!mapping_inserted)
            {
                (void)static_iterator;
                owner->static_heightfields.erase(body_key);
                owner->physics.GetBodyInterface().DestroyBody(state.body);
                impl_->failed = true;
                impl_->failure = "Jolt body owner identity collision";
                impl_->lease->impl_->discarded_prepared =
                    std::move(impl_->prepared);
                return false;
            }
            (void)mapping_iterator;
            impl_->lease->impl_->bodies.push_back(state.body);
            fields.pop_back();
            ++staged;
        }
        return fields.empty();
    }

    std::unique_ptr<Physics3DStaticBatchLease>
    Physics3DStaticBatchStager::cancel() noexcept
    {
        if (!impl_)
        {
            return {};
        }
        if (impl_->prepared)
        {
            if (impl_->lease->impl_->discarded_prepared)
            {
                std::abort();
            }
            impl_->lease->impl_->discarded_prepared =
                std::move(impl_->prepared);
        }
        return std::move(impl_->lease);
    }

    std::unique_ptr<Physics3DStaticBatchLease>
    Physics3DStaticBatchStager::finish() noexcept
    {
        if (!impl_ || impl_->failed || !impl_->prepared || !impl_->lease ||
            !impl_->prepared->impl_->fields.empty())
        {
            return {};
        }
        impl_->prepared.reset();
        return std::move(impl_->lease);
    }

    std::uint32_t
    Physics3DStaticBatchStager::remainingBodies() const noexcept
    {
        if (!impl_)
        {
            return 0u;
        }
        if (impl_->failed)
        {
            return impl_->lease ? impl_->lease->remainingBodies() : 0u;
        }
        if (!impl_->prepared)
        {
            return 0u;
        }
        const auto remaining = impl_->prepared->impl_->fields.size();
        return static_cast<std::uint32_t>(remaining);
    }

    lux::cxx::expected<std::shared_ptr<Physics3DScene>, std::string>
    Physics3DScene::create(Physics3DConfig config) noexcept
    {
        if (!config.gravity.allFinite() || !finitePositive(config.fixed_dt) ||
            !finitePositive(config.max_accumulated) ||
            config.max_substeps == 0u || config.maximum_bodies < 1024u ||
            config.maximum_body_pairs < config.maximum_bodies ||
            config.maximum_contact_constraints == 0u ||
            config.temporary_allocator_bytes < 8u * 1024u * 1024u ||
            config.temporary_allocator_bytes > 512u * 1024u * 1024u)
        {
            return lux::cxx::unexpected(
                std::string{"invalid Physics3D configuration"});
        }
        return std::shared_ptr<Physics3DScene>{
            new Physics3DScene{
                std::make_unique<Impl>(std::move(config))}};
    }

    Physics3DScene::Physics3DScene(std::unique_ptr<Impl> impl) noexcept
        : impl_(std::move(impl))
    {}

    Physics3DScene::~Physics3DScene() noexcept = default;

    void Physics3DScene::advance(
        lux::meta::EntityRegistry& registry,
        float frame_dt) noexcept
    {
        if (!impl_ || !std::isfinite(frame_dt) || frame_dt <= 0.0f)
        {
            return;
        }
        impl_->pruneBodies(registry);
        impl_->pruneCharacters(registry);
        impl_->ensureBodies(registry);
        impl_->ensureCharacters(registry);
        impl_->accumulator = std::min(
            impl_->accumulator + frame_dt, impl_->config.max_accumulated);
        std::uint32_t steps = 0u;
        while (impl_->accumulator >= impl_->config.fixed_dt &&
               steps < impl_->config.max_substeps)
        {
            impl_->syncKinematics(registry);
            impl_->stepCharacters(registry, impl_->config.fixed_dt);
            (void)impl_->physics.Update(
                impl_->config.fixed_dt, 1, &impl_->scratch, &impl_->jobs);
            // Jolt's barrier is allowed to steal and finish a queued Job on
            // the waiting thread. The corresponding TBB wrapper can therefore
            // still be pending even though PhysicsSystem::Update returned.
            // Join those wrappers while the JobSystem is alive so their final
            // Release cannot call FreeJob through a destroyed owner.
            impl_->jobs.settle();
            impl_->accumulator -= impl_->config.fixed_dt;
            ++steps;
        }
        if (steps == impl_->config.max_substeps &&
            impl_->accumulator >= impl_->config.fixed_dt)
            impl_->accumulator = std::fmod(
                impl_->accumulator, impl_->config.fixed_dt);
        impl_->collectContacts(registry);
        impl_->scatter(registry);
    }

    Physics3DExp<std::unique_ptr<Physics3DStaticBatchStager>>
    Physics3DScene::beginStaticHeightfieldStaging(
        std::unique_ptr<Physics3DPreparedStaticBatch> prepared,
        entt::entity owner) noexcept
    {
        if (!impl_ || !prepared || !prepared->impl_ ||
            prepared->impl_->fields.empty() || owner == entt::null)
        {
            return lux::cxx::unexpected(
                std::string{"static heightfield preparation is invalid"});
        }
        const auto& fields = prepared->impl_->fields;
        const bool origin_ready = impl_->establishStaticStagingOrigin(
            fields.front().origin);
        auto lease_impl =
            std::make_unique<Physics3DStaticBatchLease::Impl>();
        lease_impl->control = impl_->control;
        lease_impl->generation = impl_->control->generation;
        lease_impl->owner = owner;
        lease_impl->bodies.reserve(fields.size());
        auto stager_impl =
            std::make_unique<Physics3DStaticBatchStager::Impl>();
        stager_impl->lease = std::unique_ptr<Physics3DStaticBatchLease>{
            new Physics3DStaticBatchLease{std::move(lease_impl)}};
        if (origin_ready)
        {
            stager_impl->prepared = std::move(prepared);
        }
        else
        {
            // Keep even a rejected preparation behind the bounded lease.
            // Returning an error here would destroy every immutable Jolt
            // shape in this owner update.
            stager_impl->failed = true;
            stager_impl->failure =
                "static heightfield batch cannot share PhysicsOrigin";
            stager_impl->lease->impl_->discarded_prepared =
                std::move(prepared);
        }
        return std::unique_ptr<Physics3DStaticBatchStager>{
            new Physics3DStaticBatchStager{std::move(stager_impl)}};
    }

    std::unique_ptr<Physics3DStaticBatchLease>
    Physics3DScene::makeStaticHeightfieldRetirement(
        std::unique_ptr<Physics3DPreparedStaticBatch> prepared,
        entt::entity owner) noexcept
    {
        if (!impl_ || !prepared || !prepared->impl_ ||
            prepared->impl_->fields.empty() || owner == entt::null)
        {
            return {};
        }
        auto lease_impl =
            std::make_unique<Physics3DStaticBatchLease::Impl>();
        lease_impl->control = impl_->control;
        lease_impl->generation = impl_->control->generation;
        lease_impl->owner = owner;
        lease_impl->discarded_prepared = std::move(prepared);
        return std::unique_ptr<Physics3DStaticBatchLease>{
            new Physics3DStaticBatchLease{std::move(lease_impl)}};
    }

    std::span<const Physics3DContactFact>
    Physics3DScene::contacts() const noexcept
    {
        return impl_ ? std::span<const Physics3DContactFact>{impl_->contacts} :
            std::span<const Physics3DContactFact>{};
    }

    std::uint32_t Physics3DScene::dynamicBodyCount() const noexcept
    {
        return impl_ ? static_cast<std::uint32_t>(
            impl_->dynamic_bodies.size()) : 0u;
    }

    std::uint32_t Physics3DScene::characterCount() const noexcept
    {
        return impl_ ? static_cast<std::uint32_t>(
            impl_->characters.size()) : 0u;
    }

    std::uint32_t Physics3DScene::staticHeightfieldBodyCount() const noexcept
    {
        return impl_ ? static_cast<std::uint32_t>(
            impl_->static_heightfields.size()) : 0u;
    }

    std::uint64_t Physics3DScene::droppedContactFactCount() const noexcept
    {
        return impl_ ? impl_->contact_listener.dropped() : 0u;
    }

    Physics3DMemorySnapshot Physics3DScene::memorySnapshot() const noexcept
    {
        Physics3DMemorySnapshot result{};
        if (!impl_)
        {
            return result;
        }

        result.capacity_bytes = sizeof(Impl) +
            impl_->config.temporary_allocator_bytes;
        result.allocation_count = 2u; // Impl + fixed scratch allocation.
        const auto addAllocation = [&result](std::uint64_t bytes) noexcept
        {
            if (bytes == 0u)
            {
                return;
            }
            result.capacity_bytes += bytes;
            ++result.allocation_count;
        };
        const auto addMap = [&addAllocation](const auto& values)
        {
            using Map = std::remove_cvref_t<decltype(values)>;
            addAllocation(values.bucket_count() * sizeof(void*));
            if (!values.empty())
            {
                addAllocation(values.size() * sizeof(
                    typename Map::value_type));
            }
        };
        addMap(impl_->dynamic_bodies);
        addMap(impl_->characters);
        addMap(impl_->body_to_entity);
        addMap(impl_->static_heightfields);
        addAllocation(
            impl_->contacts.capacity() * sizeof(Physics3DContactFact));
        addAllocation(
            impl_->raw_contacts.capacity() * sizeof(Impl::Contacts::RawFact));
        addAllocation(impl_->contact_listener.capacityBytes());
        return result;
    }
} // namespace lux::ecs
