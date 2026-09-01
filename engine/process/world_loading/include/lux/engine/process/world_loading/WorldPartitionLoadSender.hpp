#pragma once

#include <lux/engine/partition/PartitionOrdinal.hpp>
#include <lux/engine/process/world_loading/WorldStorageSource.hpp>
#include <lux/engine/world/WorldPartitionData.hpp>

#include <cstddef>
#include <memory>
#include <stop_token>
#include <stdexec/execution.hpp>
#include <type_traits>
#include <utility>

namespace lux::process::world_loading
{
    namespace detail
    {
        class LUX_ENGINE_PROCESS_WORLD_LOADING_PUBLIC WorldPartitionLoadMachine final
        {
        public:
            WorldPartitionLoadMachine(
                WorldStorageSource source,
                lux::partition::PartitionOrdinal partition,
                std::size_t max_bytes,
                std::stop_token stop,
                void* receiver,
                void (*set_value)(void*, lux::world::WorldPartitionData&&) noexcept,
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
                stdexec::set_value_t(lux::world::WorldPartitionData),
                stdexec::set_error_t(WorldStorageRuntimeFailure),
                stdexec::set_stopped_t()
            >;

            WorldPartitionLoadSender(
                WorldStorageSource source,
                lux::partition::PartitionOrdinal partition,
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
                    lux::partition::PartitionOrdinal partition,
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
                          [](void* state, lux::world::WorldPartitionData&& value) noexcept {
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
            lux::partition::PartitionOrdinal partition_;
            std::size_t max_bytes_{};
            std::stop_token stop_;
        };
    }

    [[nodiscard]] inline auto loadWorldPartition(
        WorldStorageSource source,
        lux::partition::PartitionOrdinal partition,
        std::size_t max_bytes,
        std::stop_token stop
    ) noexcept
    {
        return detail::WorldPartitionLoadSender(std::move(source), partition, max_bytes, stop);
    }
}
