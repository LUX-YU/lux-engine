#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/WorldRuntime.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/storage/detail/WorldStorageCodec.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <stdexec/execution.hpp>
#include <stop_token>
#include <utility>
#include <vector>

namespace
{
    using namespace lux;
    using namespace lux::world;
    using namespace lux::world::detail;

    template <class Type>
    [[nodiscard]] Type id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[15] = tail;
        return Type{uuids::uuid(bytes)};
    }

    class DeferredMemoryEndpoint final
        : public async::OperationPort<scene::ReadWorldStorageRange>::Endpoint
    {
    public:
        struct Request final
        {
            scene::ReadWorldStorageRange operation;
            std::size_t accounted_bytes{};
        };

        explicit DeferredMemoryEndpoint(std::vector<std::vector<std::byte>> volumes)
            : volumes_(std::move(volumes))
        {
        }

        [[nodiscard]] async::SubmitResult submit(
            scene::ReadWorldStorageRange operation,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            async::SubmitOptions options
        ) noexcept override
        {
            assert(!pending_);
            requests.push_back({operation, options.accounted_bytes});
            pending_ = Pending{operation, state, complete};
            return {};
        }

        [[nodiscard]] bool pending() const noexcept
        {
            return pending_.has_value();
        }

        void completeNext()
        {
            assert(pending_);
            Pending pending = *std::exchange(pending_, std::nullopt);
            if (before_complete)
                before_complete();
            const auto& operation = pending.operation;
            if (operation.volume >= volumes_.size() ||
                operation.offset > volumes_[operation.volume].size() ||
                operation.size > volumes_[operation.volume].size() - operation.offset)
            {
                pending.complete(
                    pending.state,
                    lux::cxx::unexpected(
                        async::OperationFailure<scene::WorldStorageRuntimeFailure>::domain(
                            {scene::EWorldStorageRuntimeError::RANGE_OVERFLOW, operation.volume, operation.offset}
                        )
                    )
                );
                return;
            }
            pending.complete(
                pending.state,
                Outcome{lux::cxx::SharedBytes<>::copyOf(
                    std::span<const std::byte>(volumes_[operation.volume]).subspan(
                        static_cast<std::size_t>(operation.offset),
                        static_cast<std::size_t>(operation.size)
                    )
                )}
            );
        }

        std::vector<Request> requests;
        std::function<void()> before_complete;

    private:
        struct Pending final
        {
            scene::ReadWorldStorageRange operation;
            void* state{};
            void (*complete)(void*, Outcome&&) noexcept{};
        };

        std::vector<std::vector<std::byte>> volumes_;
        std::optional<Pending> pending_;
    };

    struct ReceiverState final
    {
        std::optional<WorldPartitionData> value;
        std::optional<scene::WorldStorageRuntimeFailure> error;
        bool stopped{};
        bool delivered{};
        bool completed{};
    };

    struct Receiver final
    {
        using receiver_concept = stdexec::receiver_t;

        void set_value(WorldPartitionData value) && noexcept
        {
            if (!require_requester || !requester.expired())
            {
                state->value.emplace(std::move(value));
                state->delivered = true;
            }
            state->completed = true;
        }

        void set_error(scene::WorldStorageRuntimeFailure error) && noexcept
        {
            state->error = error;
            state->completed = true;
        }

        void set_stopped() && noexcept
        {
            state->stopped = true;
            state->completed = true;
        }

        [[nodiscard]] stdexec::empty_env get_env() const noexcept
        {
            return {};
        }

        ReceiverState* state{};
        std::weak_ptr<int> requester;
        bool require_requester{};
    };

    struct Fixture final
    {
        WorldBundleId bundle{id<WorldBundleId>(1U)};
        WorldBundleGeneration generation{id<WorldBundleGeneration>(2U)};
        std::vector<std::vector<std::byte>> volumes;
        std::shared_ptr<const WorldDescription> world;
        std::size_t single_chunk_limit{};

        Fixture()
        {
            const std::array objects{
                WorldEncodedObjectRecord{id<WorldObjectId>(1U), {}},
                WorldEncodedObjectRecord{id<WorldObjectId>(2U), {}}
            };
            auto partition = encodeWorldPartitionData(WorldPartitionOrdinal{0U}, objects);
            assert(partition);
            const std::size_t split = partition->size() / 2U;
            std::vector<std::byte> first(partition->begin(), partition->begin() + split);
            std::vector<std::byte> second(partition->begin() + split, partition->end());

            const std::array records{
                WorldPartitionRecord{id<WorldPartitionId>(1U), 0U, 2U}
            };
            const std::array extents{
                WorldPartitionExtent{0U, 1U, 1U},
                WorldPartitionExtent{1U, 0U, 1U}
            };
            auto page = encodeWorldPartitionTablePage(WorldPartitionOrdinal{0U}, records, extents);
            assert(page);
            single_chunk_limit = std::max({
                kWorldStorageVolumeHeaderWireSize,
                kWorldStorageChunkDescriptorWireSize,
                page->size(),
                first.size(),
                second.size()
            });
            assert(single_chunk_limit < partition->size());

            const std::array volume0_chunks{
                WorldStorageChunkInput{
                    EWorldStorageChunkKind::PARTITION_TABLE_PAGE,
                    EWorldStorageCodec::NONE,
                    *page
                },
                WorldStorageChunkInput{
                    EWorldStorageChunkKind::WORLD_PARTITION_DATA,
                    EWorldStorageCodec::NONE,
                    first
                }
            };
            const std::array volume1_chunks{
                WorldStorageChunkInput{
                    EWorldStorageChunkKind::WORLD_PARTITION_DATA,
                    EWorldStorageCodec::NONE,
                    second
                }
            };
            auto volume0 = encodeWorldStorageVolume(bundle, generation, 0U, volume0_chunks);
            auto volume1 = encodeWorldStorageVolume(bundle, generation, 1U, volume1_chunks);
            assert(volume0 && volume1);
            volumes.push_back(std::move(*volume0));
            volumes.push_back(std::move(*volume1));
            world = buildWorld(generation, 2U);
        }

        [[nodiscard]] std::shared_ptr<const WorldDescription> buildWorld(
            WorldBundleGeneration world_generation,
            std::uint32_t first_volume_chunks
        ) const
        {
            WorldDescriptionBuilder builder;
            assert(builder.setIdentity(bundle, world_generation, "deferred-world"));
            assert(builder.addSchema(worldDataSchemaId("test.empty")));
            assert(builder.setPartitioner({worldPartitionerId("test.deferred"), 1U}, 1U));
            assert(builder.addStorageVolume({
                "deferred.wvol0",
                1U,
                first_volume_chunks,
                volumes[0].size()
            }));
            assert(builder.addStorageVolume({
                "deferred.wvol1",
                1U,
                1U,
                volumes[1].size()
            }));
            assert(builder.addPartitionTablePage({WorldPartitionOrdinal{0U}, 1U, {0U, 0U}}));
            auto result = std::move(builder).build();
            assert(result);
            return std::make_shared<WorldDescription>(std::move(*result));
        }

        [[nodiscard]] scene::WorldStorageSource source(
            const std::shared_ptr<DeferredMemoryEndpoint>& endpoint,
            std::shared_ptr<const WorldDescription> selected_world = {}
        ) const
        {
            auto result = scene::WorldStorageSource::create(
                selected_world ? std::move(selected_world) : world,
                async::OperationPort<scene::ReadWorldStorageRange>{endpoint}
            );
            assert(result);
            return *result;
        }
    };

    template <class State>
    void drive(DeferredMemoryEndpoint& endpoint, State& state)
    {
        while (!state.completed)
        {
            assert(endpoint.pending());
            endpoint.completeNext();
        }
    }
}

int main()
{
    Fixture fixture;

    {
        auto endpoint = std::make_shared<DeferredMemoryEndpoint>(fixture.volumes);
        ReceiverState result;
        auto operation = stdexec::connect(
            scene::loadWorldPartition(
                fixture.source(endpoint),
                WorldPartitionOrdinal{0U},
                fixture.volumes[0].size() + fixture.volumes[1].size(),
                {}
            ),
            Receiver{&result}
        );
        stdexec::start(operation);
        drive(*endpoint, result);
        assert(result.value);
        assert(result.value->bundle() == fixture.bundle);
        assert(result.value->generation() == fixture.generation);
        assert(result.value->objectCount() == 2U);
        bool saw_volume0{};
        bool saw_volume1{};
        for (const auto& request : endpoint->requests)
        {
            assert(request.operation.size == request.accounted_bytes);
            saw_volume0 = saw_volume0 || request.operation.volume == 0U;
            saw_volume1 = saw_volume1 || request.operation.volume == 1U;
        }
        assert(saw_volume0 && saw_volume1);
    }

    {
        const auto bundle = id<WorldBundleId>(31U);
        const auto generation = id<WorldBundleGeneration>(32U);
        const std::array records{
            WorldPartitionRecord{id<WorldPartitionId>(33U), 0U, 1U}
        };
        const std::array extents{
            WorldPartitionExtent{0U, 0U, (std::numeric_limits<std::uint32_t>::max)()}
        };
        auto page = encodeWorldPartitionTablePage(WorldPartitionOrdinal{0U}, records, extents);
        assert(page);
        const std::array chunks{
            WorldStorageChunkInput{
                EWorldStorageChunkKind::PARTITION_TABLE_PAGE,
                EWorldStorageCodec::NONE,
                *page
            }
        };
        auto volume = encodeWorldStorageVolume(bundle, generation, 0U, chunks);
        assert(volume);

        WorldDescriptionBuilder builder;
        assert(builder.setIdentity(bundle, generation, "hostile-extent"));
        assert(builder.addSchema(worldDataSchemaId("test.empty")));
        assert(builder.setPartitioner({worldPartitionerId("test.hostile"), 1U}, 1U));
        assert(builder.addStorageVolume({"hostile.wvol0", 1U, 1U, volume->size()}));
        assert(builder.addPartitionTablePage({WorldPartitionOrdinal{0U}, 1U, {0U, 0U}}));
        auto world_value = std::move(builder).build();
        assert(world_value);
        auto world = std::make_shared<WorldDescription>(std::move(*world_value));
        auto endpoint = std::make_shared<DeferredMemoryEndpoint>(
            std::vector<std::vector<std::byte>>{*volume}
        );
        auto source = scene::WorldStorageSource::create(
            world,
            async::OperationPort<scene::ReadWorldStorageRange>{endpoint}
        );
        assert(source);

        ReceiverState result;
        auto operation = stdexec::connect(
            scene::loadWorldPartition(*source, WorldPartitionOrdinal{0U}, volume->size(), {}),
            Receiver{&result}
        );
        stdexec::start(operation);
        drive(*endpoint, result);
        assert(result.error);
        assert(result.error->code == scene::EWorldStorageRuntimeError::CORRUPT_DESCRIPTOR);
        assert(endpoint->requests.size() == 3U);
    }

    {
        auto endpoint = std::make_shared<DeferredMemoryEndpoint>(fixture.volumes);
        std::stop_source stop;
        ReceiverState result;
        auto operation = stdexec::connect(
            scene::loadWorldPartition(
                fixture.source(endpoint),
                WorldPartitionOrdinal{0U},
                fixture.volumes[0].size() + fixture.volumes[1].size(),
                stop.get_token()
            ),
            Receiver{&result}
        );
        stdexec::start(operation);
        assert(endpoint->pending());
        stop.request_stop();
        endpoint->completeNext();
        assert(result.stopped);
        assert(!endpoint->pending());
    }

    {
        auto endpoint = std::make_shared<DeferredMemoryEndpoint>(fixture.volumes);
        ReceiverState result;
        auto operation = stdexec::connect(
            scene::loadWorldPartition(
                fixture.source(endpoint),
                WorldPartitionOrdinal{0U},
                fixture.single_chunk_limit,
                {}
            ),
            Receiver{&result}
        );
        stdexec::start(operation);
        drive(*endpoint, result);
        assert(result.error);
        assert(result.error->code == scene::EWorldStorageRuntimeError::LIMIT_EXCEEDED);
    }

    {
        auto endpoint = std::make_shared<DeferredMemoryEndpoint>(fixture.volumes);
        ReceiverState result;
        auto stale_world = fixture.buildWorld(id<WorldBundleGeneration>(3U), 2U);
        auto operation = stdexec::connect(
            scene::loadWorldPartition(
                fixture.source(endpoint, stale_world),
                WorldPartitionOrdinal{0U},
                fixture.volumes[0].size() + fixture.volumes[1].size(),
                {}
            ),
            Receiver{&result}
        );
        stdexec::start(operation);
        endpoint->completeNext();
        assert(result.error);
        assert(result.error->code == scene::EWorldStorageRuntimeError::BUNDLE_MISMATCH);
    }

    {
        auto endpoint = std::make_shared<DeferredMemoryEndpoint>(fixture.volumes);
        ReceiverState result;
        auto wrong_chunks = fixture.buildWorld(fixture.generation, 3U);
        auto operation = stdexec::connect(
            scene::loadWorldPartition(
                fixture.source(endpoint, wrong_chunks),
                WorldPartitionOrdinal{0U},
                fixture.volumes[0].size() + fixture.volumes[1].size(),
                {}
            ),
            Receiver{&result}
        );
        stdexec::start(operation);
        endpoint->completeNext();
        assert(result.error);
        assert(result.error->code == scene::EWorldStorageRuntimeError::CORRUPT_DESCRIPTOR);
    }

    {
        simulation::SimulationDescriptionBuilder simulation_builder;
        auto simulation = std::move(simulation_builder).build();
        assert(simulation);
        simulation::SystemRegistry systems;
        auto scene_result = scene::Scene::create(
            fixture.world,
            std::make_shared<simulation::SimulationDescription>(std::move(*simulation)),
            systems
        );
        assert(scene_result);
        auto scene_owner = std::move(*scene_result);

        auto endpoint = std::make_shared<DeferredMemoryEndpoint>(fixture.volumes);
        ReceiverState result;
        auto operation = stdexec::connect(
            scene::loadWorldPartition(
                fixture.source(endpoint),
                WorldPartitionOrdinal{0U},
                fixture.volumes[0].size() + fixture.volumes[1].size(),
                scene_owner->stopToken()
            ),
            Receiver{&result}
        );
        stdexec::start(operation);
        scene_owner->requestStop();
        scene_owner.reset();
        endpoint->completeNext();
        assert(result.stopped);
    }

    {
        auto endpoint = std::make_shared<DeferredMemoryEndpoint>(fixture.volumes);
        auto requester = std::make_shared<int>(42);
        ReceiverState result;
        auto operation = stdexec::connect(
            scene::loadWorldPartition(
                fixture.source(endpoint),
                WorldPartitionOrdinal{0U},
                fixture.volumes[0].size() + fixture.volumes[1].size(),
                {}
            ),
            Receiver{&result, requester, true}
        );
        stdexec::start(operation);
        requester.reset();
        drive(*endpoint, result);
        assert(!result.delivered);
        assert(!result.error);
        assert(!result.stopped);
    }

    return 0;
}
