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

    private:
        struct RawCommandVTable final
        {
            std::size_t size{};
            std::size_t alignment{};
            void (*move_construct)(void*, void*);
            void (*apply)(void*, Registry&, Entity);
            void (*destroy)(void*) noexcept;
        };

        EcsCommandWriter(EcsCommandBuffer& owner, std::uint32_t producer, std::uint32_t generation) noexcept;

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
                    [](void* raw) noexcept { std::destroy_at(static_cast<Payload*>(raw)); }};
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
        [[nodiscard]] lux::cxx::expected<EcsCommandWriter, EcsCommandFailure> begin(std::size_t producer) noexcept;
        [[nodiscard]] std::optional<Entity> resolve(DeferredEntity entity) const noexcept;
        [[nodiscard]] bool failed() const noexcept;
        [[nodiscard]] std::size_t allocationEvents() const noexcept;
        [[nodiscard]] std::size_t discarded() const noexcept;
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

        friend class EcsCommandWriter;
        friend LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC lux::cxx::expected<void, EcsCommandFailure>
        applyEcsCommands(Registry&, EcsCommandBuffer&) noexcept;
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC lux::cxx::expected<void, EcsCommandFailure>
    applyEcsCommands(Registry& registry, EcsCommandBuffer& commands) noexcept;
}
