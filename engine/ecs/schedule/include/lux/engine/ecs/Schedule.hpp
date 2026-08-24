#pragma once

#include <lux/engine/ecs/ScheduleError.hpp>
#include <lux/engine/ecs/System.hpp>
#include <lux/engine/ecs/SystemHandle.hpp>
#include <lux/engine/ecs/SystemPhase.hpp>
#include <lux/engine/ecs/schedule/visibility.h>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/object/LuxObject.hpp>

#include <concepts>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    namespace detail
    {
        struct ScheduleTestAccess;

        struct StagedSystemHandle final
        {
            std::uint32_t slot{};
            std::uint32_t generation{};
        };

        struct AnySystemHandle final
        {
            std::uint64_t owner{};
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
            static_assert(std::is_base_of_v<System, T>);
            if (schedule_ == nullptr || !system)
            {
                recordFailure(EScheduleError::NULL_SYSTEM);
                return {};
            }

            const auto handle = stageAdd(
                lux::cxx::typeToken<T>(),
                std::move(system),
                phase,
                std::derived_from<T, lux::object::LuxObject>
            );
            if (!handle)
                return {};
            return SystemHandle<T>(
                ownerId(),
                handle->slot,
                handle->generation
            );
        }

        template <class T>
        void remove(SystemHandle<T> handle)
        {
            stageRemove(detail::AnySystemHandle{
                handle.owner_,
                handle.slot_,
                handle.generation_});
        }

        [[nodiscard]] lux::cxx::expected<void, ScheduleFailure>
        commit() noexcept;

      private:
        explicit ScheduleEdit(Schedule& schedule);

        [[nodiscard]] lux::cxx::expected<detail::StagedSystemHandle, ScheduleFailure>
        stageAdd(
            lux::cxx::TypeToken type,
            std::unique_ptr<System> system,
            SystemPhase phase,
            bool object_affine
        ) noexcept;

        void stageRemove(detail::AnySystemHandle handle) noexcept;
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

        void requestClose() noexcept;
        void runCloseStep(float delta_seconds, std::uint64_t tick_index) noexcept;
        [[nodiscard]] bool closeComplete() const noexcept;

        template <class T>
        [[nodiscard]] T* get(SystemHandle<T> handle) noexcept
        {
            return static_cast<T*>(getRaw(
                detail::AnySystemHandle{
                    handle.owner_,
                    handle.slot_,
                    handle.generation_},
                lux::cxx::typeToken<T>()
            ));
        }

      private:
        void* getRaw(
            detail::AnySystemHandle handle,
            lux::cxx::TypeToken expected_type
        ) noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend class ScheduleEdit;
        friend struct detail::ScheduleTestAccess;
    };
} // namespace lux::ecs
