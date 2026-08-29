#pragma once

#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/scene/runtime/world/visibility.h>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/world/WorldDescription.hpp>
#include <lux/engine/world/WorldPartitionData.hpp>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <stdexec/execution.hpp>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::scene
{
    enum class EWorldStorageRuntimeError : std::uint8_t
    {
        INVALID_SOURCE,
        INVALID_PARTITION,
        INVALID_VOLUME,
        BUNDLE_MISMATCH,
        RANGE_OVERFLOW,
        LIMIT_EXCEEDED,
        IO_FAILURE,
        CORRUPT_DESCRIPTOR,
        DIGEST_MISMATCH,
        DECOMPRESSION_FAILURE,
        DECODE_FAILURE,
        ALLOCATION_FAILURE,
    };

    struct WorldStorageRuntimeFailure final
    {
        EWorldStorageRuntimeError code{EWorldStorageRuntimeError::INVALID_SOURCE};
        std::uint32_t volume{};
        std::uint64_t offset{};
    };

    struct ReadWorldStorageRange final
    {
        using Value = lux::cxx::SharedBytes<>;
        using Error = WorldStorageRuntimeFailure;

        std::uint32_t volume{};
        std::uint64_t offset{};
        std::uint64_t size{};
    };

    class LUX_ENGINE_SCENE_WORLD_RUNTIME_PUBLIC WorldStorageSource final
    {
    public:
        WorldStorageSource() noexcept = default;

        [[nodiscard]] static lux::cxx::expected<WorldStorageSource, WorldStorageRuntimeFailure> create(
            std::shared_ptr<const world::WorldDescription> world,
            lux::async::OperationPort<ReadWorldStorageRange> read_port
        ) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] const world::WorldDescription& world() const noexcept;
        [[nodiscard]] const lux::async::OperationPort<ReadWorldStorageRange>& readPort() const noexcept;

    private:
        WorldStorageSource(
            std::shared_ptr<const world::WorldDescription> world,
            lux::async::OperationPort<ReadWorldStorageRange> read_port
        ) noexcept;

        std::shared_ptr<const world::WorldDescription> world_;
        lux::async::OperationPort<ReadWorldStorageRange> read_port_;
    };

    enum class EWorldMaterializeError : std::uint8_t
    {
        INVALID_WORLD_SCHEMA,
        INVALID_OBJECT,
        COMPONENT_DECODE_FAILURE,
        ALLOCATION_FAILURE,
    };

    struct WorldMaterializeFailure final
    {
        EWorldMaterializeError code{EWorldMaterializeError::INVALID_OBJECT};
        simulation::ecs::ComponentDecodeFailure component;
        std::size_t object{};
        std::size_t data{};
    };

    class LUX_ENGINE_SCENE_WORLD_RUNTIME_PUBLIC WorldMaterializer final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<WorldMaterializer, WorldMaterializeFailure> create(
            std::shared_ptr<const world::WorldDescription> world,
            simulation::ecs::ComponentSchemaSet components
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<simulation::ecs::Entity, WorldMaterializeFailure> object(
            simulation::ecs::Registry& registry,
            world::WorldPartitionObjectView object
        ) const noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldMaterializeFailure> partition(
            simulation::ecs::Registry& registry,
            const world::WorldPartitionData& data,
            std::vector<simulation::ecs::Entity>* created = nullptr
        ) const noexcept;

    private:
        WorldMaterializer(
            std::shared_ptr<const world::WorldDescription> world,
            simulation::ecs::ComponentSchemaSet components,
            std::vector<const simulation::ecs::ComponentSchema*> mappings
        ) noexcept;

        std::shared_ptr<const world::WorldDescription> world_;
        simulation::ecs::ComponentSchemaSet components_;
        std::vector<const simulation::ecs::ComponentSchema*> mappings_;
    };

    namespace detail
    {
        class LUX_ENGINE_SCENE_WORLD_RUNTIME_PUBLIC WorldPartitionLoadMachine final
        {
        public:
            WorldPartitionLoadMachine(
                WorldStorageSource source,
                world::WorldPartitionOrdinal partition,
                std::size_t max_bytes,
                std::stop_token stop,
                void* receiver,
                void (*set_value)(void*, world::WorldPartitionData&&) noexcept,
                void (*set_error)(void*, WorldStorageRuntimeFailure) noexcept,
                void (*set_stopped)(void*) noexcept
            );
            ~WorldPartitionLoadMachine();
            WorldPartitionLoadMachine(const WorldPartitionLoadMachine&) = delete;
            WorldPartitionLoadMachine& operator=(const WorldPartitionLoadMachine&) = delete;
            WorldPartitionLoadMachine(WorldPartitionLoadMachine&&) = delete;
            WorldPartitionLoadMachine& operator=(WorldPartitionLoadMachine&&) = delete;

            void start() noexcept;

        private:
            struct Impl;
            std::unique_ptr<Impl> impl_;
        };

        class WorldPartitionLoadSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(world::WorldPartitionData),
                stdexec::set_error_t(WorldStorageRuntimeFailure),
                stdexec::set_stopped_t()
            >;

            WorldPartitionLoadSender(
                WorldStorageSource source,
                world::WorldPartitionOrdinal partition,
                std::size_t max_bytes,
                std::stop_token stop
            ) noexcept
                : source_(std::move(source)), partition_(partition), max_bytes_(max_bytes), stop_(stop)
            {
            }

            template <class Receiver>
            class State final
            {
            public:
                using operation_state_concept = stdexec::operation_state_t;

                State(
                    WorldStorageSource source,
                    world::WorldPartitionOrdinal partition,
                    std::size_t max_bytes,
                    std::stop_token stop,
                    Receiver receiver
                )
                    : receiver_(std::move(receiver)),
                      machine_(
                          std::move(source),
                          partition,
                          max_bytes,
                          stop,
                          this,
                          [](void* state, world::WorldPartitionData&& value) noexcept {
                              auto& self = *static_cast<State*>(state);
                              stdexec::set_value(std::move(self.receiver_), std::move(value));
                          },
                          [](void* state, WorldStorageRuntimeFailure failure) noexcept {
                              auto& self = *static_cast<State*>(state);
                              stdexec::set_error(std::move(self.receiver_), failure);
                          },
                          [](void* state) noexcept {
                              auto& self = *static_cast<State*>(state);
                              stdexec::set_stopped(std::move(self.receiver_));
                          }
                      )
                {
                }

                State(const State&) = delete;
                State& operator=(const State&) = delete;
                State(State&&) = delete;
                State& operator=(State&&) = delete;

                void start() & noexcept
                {
                    machine_.start();
                }

            private:
                Receiver receiver_;
                WorldPartitionLoadMachine machine_;
            };

            template <class Receiver>
            [[nodiscard]] State<std::decay_t<Receiver>> connect(Receiver&& receiver) &&
            {
                return State<std::decay_t<Receiver>>{
                    std::move(source_),
                    partition_,
                    max_bytes_,
                    stop_,
                    std::forward<Receiver>(receiver)
                };
            }

            [[nodiscard]] stdexec::empty_env get_env() const noexcept
            {
                return {};
            }

        private:
            WorldStorageSource source_;
            world::WorldPartitionOrdinal partition_;
            std::size_t max_bytes_{};
            std::stop_token stop_;
        };
    } // namespace detail

    [[nodiscard]] inline auto loadWorldPartition(
        WorldStorageSource source,
        world::WorldPartitionOrdinal partition,
        std::size_t max_bytes,
        std::stop_token stop
    ) noexcept
    {
        return detail::WorldPartitionLoadSender(std::move(source), partition, max_bytes, stop);
    }
} // namespace lux::scene
