#pragma once

#include <lux/engine/ecs/physics3d/Physics3DConfig.hpp>
#include <lux/engine/ecs/physics3d/StaticHeightfieldBatch3D.hpp>
#include <lux/engine/resource/spatial/Spatial.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/meta/LuxObject.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <memory>
#include <cstdint>
#include <span>
#include <string>

namespace lux::ecs
{
    struct Physics3DContactFact final
    {
        entt::entity first{entt::null};
        entt::entity second{entt::null};
        Eigen::Vector3f normal = Eigen::Vector3f::Zero();
        float penetration{0.0f};
    };

    struct Physics3DMemorySnapshot final
    {
        /// Known owner capacity only: scene/control objects, fixed Jolt scratch
        /// storage and engine-owned STL backing stores. It is not process RSS.
        std::uint64_t capacity_bytes{0u};
        std::uint32_t allocation_count{0u};
    };

    class Physics3DScene;
    class Physics3DPreparedStaticBatch;
    class Physics3DStaticBatchStager;

    /// Expensive immutable shape preparation.  It does not access EnTT or a
    /// scene-owned Jolt PhysicsSystem and is therefore safe on a background
    /// CPU worker that owns @p batch.  Keep this as the first declaration so
    /// the friend below inherits the public DLL linkage on Windows.
    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::cxx::expected<
        std::unique_ptr<Physics3DPreparedStaticBatch>,
        std::string>
    preparePhysics3DStaticBatch(
        StaticHeightfieldBatch3D batch) noexcept;

    /// CPU/Jolt-shape result produced without touching a live Physics3DScene.
    /// The expensive sample conversion and heightfield shape build can run on
    /// a background CPU scheduler.  Scene-owned bodies do not exist yet.
    class LUX_FUNCTION_PUBLIC Physics3DPreparedStaticBatch final
    {
    public:
        ~Physics3DPreparedStaticBatch() noexcept;
        Physics3DPreparedStaticBatch(
            Physics3DPreparedStaticBatch&&) noexcept;
        Physics3DPreparedStaticBatch& operator=(
            Physics3DPreparedStaticBatch&&) noexcept;

        Physics3DPreparedStaticBatch(
            const Physics3DPreparedStaticBatch&) = delete;
        Physics3DPreparedStaticBatch& operator=(
            const Physics3DPreparedStaticBatch&) = delete;

        [[nodiscard]] std::uint32_t heightfieldCount() const noexcept;

    private:
        friend class Physics3DScene;
        friend class Physics3DStaticBatchStager;
        friend class Physics3DStaticBatchLease;
        friend LUX_FUNCTION_PUBLIC lux::cxx::expected<
            std::unique_ptr<Physics3DPreparedStaticBatch>,
            std::string>
        preparePhysics3DStaticBatch(
            StaticHeightfieldBatch3D batch) noexcept;
        struct Impl;
        explicit Physics3DPreparedStaticBatch(
            std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    /// Prepared Jolt heightfield batch. Creation is fallible and happens
    /// during a bounded domain update. activate() publishes bodies outside
    /// the ECS command barrier; that barrier only adopts pre-armed transient
    /// component ownership.
    class LUX_FUNCTION_PUBLIC Physics3DStaticBatchLease final
    {
    public:
        ~Physics3DStaticBatchLease() noexcept;
        Physics3DStaticBatchLease(Physics3DStaticBatchLease&&) noexcept;
        Physics3DStaticBatchLease& operator=(
            Physics3DStaticBatchLease&&) noexcept;

        Physics3DStaticBatchLease(const Physics3DStaticBatchLease&) = delete;
        Physics3DStaticBatchLease& operator=(
            const Physics3DStaticBatchLease&) = delete;

        void activate() noexcept;
        /// Removes prepared bodies from collision queries immediately while
        /// retaining their backend allocation until this lease is destroyed.
        /// Idempotent so overlapping fact/binding destroy signals cannot
        /// double-remove a body.
        void deactivate() noexcept;
        /// Destroys already-hidden bodies and unadopted prepared shapes.
        /// Idempotent and kept separate from deactivate() so domain owners can
        /// model immediate hide followed by their own retirement granule.
        void retire() noexcept;
        /// Bounded retirement used by scene-domain owner queues. One unit is
        /// one body or one unadopted prepared shape. Returns true after both
        /// kinds of backend owner have been released.
        [[nodiscard]] bool retireSome(
            std::uint32_t maximum_units) noexcept;
        [[nodiscard]] bool active() const noexcept;
        [[nodiscard]] std::uint32_t remainingBodies() const noexcept;
        [[nodiscard]] std::uint32_t
        remainingRetirementUnits() const noexcept;

    private:
        friend class Physics3DScene;
        friend class Physics3DStaticBatchStager;
        struct Impl;
        explicit Physics3DStaticBatchLease(
            std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    /// Main-owner incremental adoption of background-prepared shapes.  Each
    /// advance creates at most the caller-provided number of Jolt bodies; all
    /// staged bodies remain invisible until an ordinary domain update
    /// explicitly activates the resulting lease.
    class LUX_FUNCTION_PUBLIC Physics3DStaticBatchStager final
    {
    public:
        ~Physics3DStaticBatchStager() noexcept;
        Physics3DStaticBatchStager(
            Physics3DStaticBatchStager&&) noexcept;
        Physics3DStaticBatchStager& operator=(
            Physics3DStaticBatchStager&&) noexcept;

        Physics3DStaticBatchStager(
            const Physics3DStaticBatchStager&) = delete;
        Physics3DStaticBatchStager& operator=(
            const Physics3DStaticBatchStager&) = delete;

        [[nodiscard]] lux::cxx::expected<bool, std::string>
        advance(std::uint32_t maximum_bodies) noexcept;
        /// Transfers every already-created invisible body to the caller so a
        /// domain retirement queue can destroy it under its ordinary granule.
        [[nodiscard]] std::unique_ptr<Physics3DStaticBatchLease>
        cancel() noexcept;
        [[nodiscard]] std::unique_ptr<Physics3DStaticBatchLease>
        finish() noexcept;
        [[nodiscard]] std::uint32_t remainingBodies() const noexcept;

    private:
        friend class Physics3DScene;
        struct Impl;
        explicit Physics3DStaticBatchStager(
            std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    /// One scene-scoped Jolt world. The header contains no JPH type; all solver
    /// objects, handles, filters, jobs and contact listeners stay in the DLL.
    class LUX_FUNCTION_PUBLIC Physics3DScene final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<Physics3DScene>,
            std::string>
        create(Physics3DConfig config = {}) noexcept;

        ~Physics3DScene() noexcept;
        Physics3DScene(const Physics3DScene&) = delete;
        Physics3DScene& operator=(const Physics3DScene&) = delete;

        void advance(
            lux::meta::EntityRegistry& registry,
            float frame_dt) noexcept;

        [[nodiscard]] lux::cxx::expected<
            std::unique_ptr<Physics3DStaticBatchStager>,
            std::string>
        beginStaticHeightfieldStaging(
            std::unique_ptr<Physics3DPreparedStaticBatch> prepared,
            entt::entity owner) noexcept;

        /// Converts an unadopted preparation into the same bounded retirement
        /// owner used by a canceled stager.  This is the stale-completion and
        /// close path: no backend body is created and each prepared shape is
        /// released by Physics3DStaticBatchLease::retireSome().
        [[nodiscard]] std::unique_ptr<Physics3DStaticBatchLease>
        makeStaticHeightfieldRetirement(
            std::unique_ptr<Physics3DPreparedStaticBatch> prepared,
            entt::entity owner) noexcept;

        [[nodiscard]] std::span<const Physics3DContactFact>
        contacts() const noexcept;
        [[nodiscard]] std::uint32_t dynamicBodyCount() const noexcept;
        [[nodiscard]] std::uint32_t characterCount() const noexcept;
        [[nodiscard]] std::uint32_t staticHeightfieldBodyCount() const noexcept;
        [[nodiscard]] std::uint64_t droppedContactFactCount() const noexcept;
        [[nodiscard]] Physics3DMemorySnapshot memorySnapshot() const noexcept;

    private:
        friend class Physics3DStaticBatchLease;
        friend class Physics3DStaticBatchStager;
        struct Impl;
        explicit Physics3DScene(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::ecs
