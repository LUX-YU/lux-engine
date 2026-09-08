#pragma once

#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/ecs/core/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lux::simulation::ecs
{
    enum class EEcsCommandPolicy : std::uint8_t
    {
        ABORT_BATCH,
        CONTINUE_ON_INVALID_TARGET,
    };

    struct EcsCommandProducerCapacity final
    {
        std::size_t max_commands;
        std::size_t max_payload_bytes;
    };

    struct DeferredEntity final
    {
        std::uint32_t producer{std::numeric_limits<std::uint32_t>::max()};
        std::uint32_t ordinal{std::numeric_limits<std::uint32_t>::max()};
        std::uint32_t generation{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return generation != 0U && producer != std::numeric_limits<std::uint32_t>::max() &&
                   ordinal != std::numeric_limits<std::uint32_t>::max();
        }
    };

    enum class EEcsCommandError : std::uint8_t
    {
        ACTIVE_WRITER,
        INVALID_PRODUCER,
        STALE_WRITER,
        RECORDING_FAILED,
        CAPACITY_EXCEEDED,
        INVALID_ENTITY,
        INVALID_DEFERRED_ENTITY,
        COMPONENT_CONSTRUCTION_FAILURE,
        ALLOCATION_FAILURE,
        MISSING_COMPONENT,
        EXISTING_COMPONENT,
    };

    struct EcsCommandFailure final
    {
        EEcsCommandError code{EEcsCommandError::RECORDING_FAILED};
        std::size_t producer{};
        std::size_t command{};
    };

    class EcsCommandBuffer;

    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsCommandWriter final
    {
    public:
        EcsCommandWriter() noexcept = default;
        ~EcsCommandWriter() noexcept;
        EcsCommandWriter(EcsCommandWriter&& other) noexcept;
        EcsCommandWriter& operator=(EcsCommandWriter&&) noexcept = delete;
        EcsCommandWriter(const EcsCommandWriter&) = delete;
        EcsCommandWriter& operator=(const EcsCommandWriter&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        // Owner-only preflight. Failure does not poison commands that were already accepted.
        [[nodiscard]] bool canRecord(std::size_t bytes = 0U, std::size_t alignment = 1U) const noexcept;
        [[nodiscard]] EEcsCommandPolicy policy() const noexcept { return policy_; }
        [[nodiscard]] DeferredEntity create() noexcept;
        void destroy(Entity entity) noexcept;

        template <class Component, class... Args> [[nodiscard]] bool emplace(Entity entity, Args&&... args) noexcept
        {
            return emplaceImpl<Component>(entity, {}, false, std::forward<Args>(args)...);
        }

        template <class Component, class... Args>
        [[nodiscard]] bool emplace(DeferredEntity entity, Args&&... args) noexcept
        {
            return emplaceImpl<Component>(NullEntity, entity, true, std::forward<Args>(args)...);
        }

        template <class Component> [[nodiscard]] bool remove(Entity entity) noexcept
        {
            return recordRemove(entity, [](Registry& registry, Entity target) {
                registry.template remove<Component>(target);
            }
            );
        }

        // Own the replacement value until the existing command barrier. Reads in this region see the old value.
        template <class Component> [[nodiscard]] bool replace(Entity entity, Component value) noexcept
        {
            static_assert(std::is_nothrow_move_constructible_v<Component>);
            static_assert(std::is_nothrow_move_assignable_v<Component>);
            static_assert(std::is_nothrow_destructible_v<Component>);
            const RawCommandVTable table{
                sizeof(Component), alignof(Component),
                [](void* target, void* source) noexcept {
                    std::construct_at(static_cast<Component*>(target), std::move(*static_cast<Component*>(source)));
                },
                [](void* raw, Registry& registry, Entity target) {
                    registry.template replace<Component>(target, std::move(*static_cast<Component*>(raw)));
                },
                [](void* raw) noexcept { std::destroy_at(static_cast<Component*>(raw)); },
                [](const Registry& registry, Entity target) noexcept {
                    return registry.template all_of<Component>(target);
                }
            };
            return recordPayload(entity, {}, false, table, std::addressof(value));
        }

    private:
        struct RawCommandVTable final
        {
            std::size_t size{};
            std::size_t alignment{};
            void (*move_construct)(void*, void*);
            void (*apply)(void*, Registry&, Entity);
            void (*destroy)(void*) noexcept;
            bool (*applicable)(const Registry&, Entity) noexcept{};
            EEcsCommandError inapplicable_error{EEcsCommandError::MISSING_COMPONENT};
        };

        EcsCommandWriter(EcsCommandBuffer& owner, std::uint32_t producer, std::uint32_t generation,
            EEcsCommandPolicy policy) noexcept;

        template <class Component, class... Args>
        [[nodiscard]] bool
        emplaceImpl(Entity entity, DeferredEntity deferred, bool uses_deferred, Args&&... args) noexcept
        {
            using Payload = std::tuple<std::decay_t<Args>...>;
            static_assert(std::is_nothrow_destructible_v<Payload>);
            try
            {
                Payload payload(std::forward<Args>(args)...);
                const RawCommandVTable table{
                    sizeof(Payload),
                    alignof(Payload),
                    [](void* target, void* source) {
                        std::construct_at(static_cast<Payload*>(target), std::move(*static_cast<Payload*>(source)));
                    },
                    [](void* raw, Registry& registry, Entity target) {
                        auto& values = *static_cast<Payload*>(raw);
                        std::apply(
                            [&](auto&... value) { registry.template emplace<Component>(target, std::move(value)...); },
                            values
                        );
                    },
                    [](void* raw) noexcept { std::destroy_at(static_cast<Payload*>(raw)); },
                    [](const Registry& registry, Entity target) noexcept {
                        return !registry.template all_of<Component>(target);
                    },
                    EEcsCommandError::EXISTING_COMPONENT
                };
                return recordPayload(entity, deferred, uses_deferred, table, std::addressof(payload));
            }
            catch (const std::bad_alloc&)
            {
                fail(EEcsCommandError::ALLOCATION_FAILURE);
                return false;
            }
            catch (...)
            {
                fail(EEcsCommandError::COMPONENT_CONSTRUCTION_FAILURE);
                return false;
            }
        }

        using RemoveFn = void (*)(Registry&, Entity);
        [[nodiscard]] bool recordPayload(
            Entity entity,
            DeferredEntity deferred,
            bool uses_deferred,
            const RawCommandVTable& table,
            void* source
        ) noexcept;
        [[nodiscard]] bool recordRemove(Entity entity, RemoveFn remove) noexcept;
        void fail(EEcsCommandError error) noexcept;
        void release() noexcept;

        EcsCommandBuffer* owner_{};
        std::uint32_t producer_{};
        std::uint32_t generation_{};
        EEcsCommandPolicy policy_{EEcsCommandPolicy::ABORT_BATCH};
        friend class EcsCommandBuffer;
    };

    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsCommandBuffer final
    {
    public:
        EcsCommandBuffer();
        ~EcsCommandBuffer();
        EcsCommandBuffer(EcsCommandBuffer&&) noexcept;
        EcsCommandBuffer& operator=(EcsCommandBuffer&&) noexcept;
        EcsCommandBuffer(const EcsCommandBuffer&) = delete;
        EcsCommandBuffer& operator=(const EcsCommandBuffer&) = delete;

        [[nodiscard]] lux::cxx::expected<void, EcsCommandFailure>
        prepare(std::span<const EcsCommandProducerCapacity> capacities) noexcept;
        void reset() noexcept;
        [[nodiscard]] lux::cxx::expected<EcsCommandWriter, EcsCommandFailure> begin(std::size_t producer,
            EEcsCommandPolicy policy = EEcsCommandPolicy::ABORT_BATCH) noexcept;
        [[nodiscard]] std::optional<Entity> resolve(DeferredEntity entity) const noexcept;
        [[nodiscard]] bool failed() const noexcept;
        [[nodiscard]] std::optional<EcsCommandFailure> producerFailure(std::size_t producer) const noexcept;
        [[nodiscard]] std::size_t allocationEvents() const noexcept;
        [[nodiscard]] std::size_t discarded() const noexcept;
        [[nodiscard]] std::size_t rejectedAtCommit() const noexcept;
        [[nodiscard]] std::optional<EcsCommandFailure> lastRejection() const noexcept;
        void discardPending() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        [[nodiscard]] DeferredEntity recordCreate(std::uint32_t producer, std::uint32_t generation) noexcept;
        void recordDestroy(std::uint32_t producer, std::uint32_t generation, Entity entity) noexcept;
        [[nodiscard]] bool recordPayload(
            std::uint32_t producer,
            std::uint32_t generation,
            Entity entity,
            DeferredEntity deferred,
            bool uses_deferred,
            const EcsCommandWriter::RawCommandVTable& table,
            void* source
        ) noexcept;
        [[nodiscard]] bool recordRemove(
            std::uint32_t producer,
            std::uint32_t generation,
            Entity entity,
            EcsCommandWriter::RemoveFn remove
        ) noexcept;
        void fail(std::uint32_t producer, std::uint32_t generation, EEcsCommandError error) noexcept;
        void end(std::uint32_t producer, std::uint32_t generation) noexcept;
        [[nodiscard]] bool writerValid(std::uint32_t producer, std::uint32_t generation) const noexcept;
        [[nodiscard]] bool canRecord(std::uint32_t producer, std::uint32_t generation,
            std::size_t bytes, std::size_t alignment) const noexcept;

        friend class EcsCommandWriter;
        friend LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC lux::cxx::expected<void, EcsCommandFailure>
        applyEcsCommands(Registry&, EcsCommandBuffer&) noexcept;
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC lux::cxx::expected<void, EcsCommandFailure>
    applyEcsCommands(Registry& registry, EcsCommandBuffer& commands) noexcept;
}
