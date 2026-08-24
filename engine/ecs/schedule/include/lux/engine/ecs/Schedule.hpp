#pragma once

#include <lux/engine/ecs/ScheduleError.hpp>
#include <lux/engine/ecs/System.hpp>
#include <lux/engine/ecs/SystemHandle.hpp>
#include <lux/engine/ecs/SystemPhase.hpp>
#include <lux/engine/ecs/SystemSetId.hpp>
#include <lux/engine/ecs/schedule/visibility.h>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <concepts>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    enum class ESystemExecutionAffinity : std::uint8_t
    {
        WORKER_ELIGIBLE,
        OWNER_THREAD,
    };

    namespace detail
    {
        struct ScheduleTestAccess;

        template <class T>
        concept ThreadAffineSystem = requires(const T& value)
        {
            typename T::lux_thread_affine;
            requires T::lux_thread_affine::value;
            { value.isOnAffinityThread() } noexcept -> std::same_as<bool>;
        };

        struct StagedSystemHandle final
        {
            std::uint32_t slot{};
            std::uint32_t generation{};
        };
    } // namespace detail

    class Schedule;

    class LUX_ENGINE_ECS_SCHEDULE_PUBLIC ScheduleEdit final
    {
      public:
        ScheduleEdit() noexcept = default;
        ScheduleEdit(const ScheduleEdit&) = delete;
        ScheduleEdit& operator=(const ScheduleEdit&) = delete;
        ScheduleEdit(ScheduleEdit&&) noexcept;
        ScheduleEdit& operator=(ScheduleEdit&&) noexcept;
        ~ScheduleEdit() noexcept;

        template <class T>
        [[nodiscard]] SystemHandle<T> add(
            std::unique_ptr<T> system,
            SystemPhase phase = SystemPhase::Update
        )
        {
            static_assert(std::derived_from<T, System>);
            if (schedule_ == nullptr || !system)
            {
                recordFailure(EScheduleError::NULL_SYSTEM);
                return {};
            }

            constexpr bool kThreadAffine = detail::ThreadAffineSystem<T>;
            bool (*validator)(const System&) noexcept{};
            if constexpr (kThreadAffine)
            {
                validator = [](const System& value) noexcept
                {
                    return static_cast<const T&>(value).isOnAffinityThread();
                };
            }

            const auto handle = stageAdd(
                lux::cxx::typeToken<T>(),
                std::move(system),
                phase,
                kThreadAffine
                    ? ESystemExecutionAffinity::OWNER_THREAD
                    : ESystemExecutionAffinity::WORKER_ELIGIBLE,
                validator
            );
            if (!handle)
                return {};
            return SystemHandle<T>(AnySystemHandle{
                ownerId(), handle->slot, handle->generation});
        }

        void addToSet(AnySystemHandle system, SystemSetId set) noexcept;
        void before(AnySystemHandle system, AnySystemHandle other) noexcept;
        void before(AnySystemHandle system, SystemSetId set) noexcept;
        void after(AnySystemHandle system, AnySystemHandle other) noexcept;
        void after(AnySystemHandle system, SystemSetId set) noexcept;
        void require(
            AnySystemHandle consumer,
            AnySystemHandle provider
        ) noexcept;
        void remove(AnySystemHandle handle) noexcept;

        [[nodiscard]] lux::cxx::expected<void, ScheduleFailure>
        commit() noexcept;

      private:
        explicit ScheduleEdit(Schedule& schedule);

        [[nodiscard]] lux::cxx::expected<detail::StagedSystemHandle, ScheduleFailure>
        stageAdd(
            lux::cxx::TypeToken type,
            std::unique_ptr<System> system,
            SystemPhase phase,
            ESystemExecutionAffinity affinity,
            bool (*affinity_validator)(const System&) noexcept
        ) noexcept;

        void recordFailure(EScheduleError error) noexcept;
        [[nodiscard]] std::uint64_t ownerId() const noexcept;
        void release() noexcept;

        struct Impl;
        Schedule* schedule_{};
        std::unique_ptr<Impl> impl_;

        friend class Schedule;
    };

    class LUX_ENGINE_ECS_SCHEDULE_PUBLIC Schedule final
    {
      public:
        explicit Schedule(World& world) noexcept;
        ~Schedule() noexcept;

        Schedule(const Schedule&) = delete;
        Schedule& operator=(const Schedule&) = delete;

        [[nodiscard]] lux::cxx::expected<ScheduleEdit, ScheduleFailure>
        edit() noexcept;

        void run(float delta_seconds, std::uint64_t tick_index) noexcept;

        [[nodiscard]] lux::cxx::expected<void, ScheduleFailure>
        requestStop(AnySystemHandle handle) noexcept;

        [[nodiscard]] bool stopped(AnySystemHandle handle) const noexcept;

        void requestClose() noexcept;
        void runCloseStep() noexcept;
        [[nodiscard]] bool closeComplete() const noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend class ScheduleEdit;
        friend struct detail::ScheduleTestAccess;
    };
} // namespace lux::ecs
