#include <array>
#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>
#include <lux/engine/process/world_loading/WorldFileStorageSource.hpp>
#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>
#include <mutex>
#include <optional>
#include <semaphore>

using namespace lux;
namespace loading = process::world_loading;

namespace
{
    template <class T> T identity(std::uint8_t value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return T{uuids::uuid{bytes}};
    }

    struct Result final
    {
        std::optional<world::WorldPartitionData> data;
        std::optional<loading::WorldStorageRuntimeFailure> error;
        std::atomic_uint completions{};
        bool stopped{};
        std::thread::id thread;
    };

    struct Receiver final
    {
        using receiver_concept = stdexec::receiver_t;
        Result& result;

        stdexec::empty_env get_env() const noexcept
        {
            return {};
        }

        void set_value(world::WorldPartitionData data) && noexcept
        {
            result.data = std::move(data);
            result.thread = std::this_thread::get_id();
            result.completions.fetch_add(1, std::memory_order_release);
        }

        void set_error(loading::WorldStorageRuntimeFailure error) && noexcept
        {
            result.error = error;
            result.completions.fetch_add(1, std::memory_order_release);
        }

        void set_stopped() && noexcept
        {
            result.stopped = true;
            result.completions.fetch_add(1, std::memory_order_release);
        }
    };

    class Delayed final : public async::TOperationPort<loading::ReadWorldStorageRange>::Endpoint
    {
    public:
        using Outcome = async::OperationOutcome<loading::ReadWorldStorageRange>;

        explicit Delayed(loading::WorldStorageSource source) : source_(std::move(source)) {}

        async::SubmitResult submit(
            loading::ReadWorldStorageRange range,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            async::SubmitOptions options
        ) noexcept override
        {
            std::lock_guard lock{mutex_};
            assert(!pending_);
            pending_ = Request{range, state, complete, options};
            return {};
        }

        void completeReady()
        {
            std::optional<Request> ready;
            {
                std::lock_guard lock{mutex_};
                ready = std::exchange(pending_, {});
            }
            if (!ready)
            {
                return;
            }
            const auto request = *ready;
            assert(source_.readPort().submit(request.range, request.state, request.complete, request.options));
            ++reads;
        }

        bool pending()
        {
            std::lock_guard lock{mutex_};
            return pending_.has_value();
        }

        unsigned reads{};

    private:
        struct Request final
        {
            loading::ReadWorldStorageRange range;
            void* state;
            void (*complete)(void*, Outcome&&) noexcept;
            async::SubmitOptions options;
        };

        loading::WorldStorageSource source_;
        std::mutex mutex_;
        std::optional<Request> pending_;
    };

    struct Fixture final
    {
        std::shared_ptr<const world::WorldDescription> world;
        std::vector<cxx::SharedBytes<>> volumes;
        std::vector<std::byte> partition;
    };

    Fixture fixture(bool bad_extent = false)
    {
        using namespace world;
        const auto bundle = identity<WorldBundleId>(1);
        const auto generation = identity<WorldBundleGeneration>(2);
        const std::array payload{std::byte{71}, std::byte{92}, std::byte{13}};
        const WorldEncodedDataRecord field{0, 91, payload}; // Unknown schema version is opaque storage data.
        const WorldEncodedObjectRecord object{identity<WorldObjectId>(3), std::span(&field, 1)};
        auto partition = encodeWorldPartitionData({0}, std::span(&object, 1));
        assert(partition);
        const auto wire = std::span(*partition);
        const auto half = wire.size() / 2;
        const WorldPartitionRecord record{identity<WorldPartitionId>(4), 0, 2};
        const std::array extents{WorldPartitionExtent{0, 1, 1}, WorldPartitionExtent{1, bad_extent ? 9U : 0U, 1}};
        auto table = encodeWorldPartitionTablePage({0}, std::span(&record, 1), extents);
        assert(table);
        const std::array first{
            WorldStorageChunkInput{EWorldStorageChunkKind::PARTITION_TABLE_PAGE, EWorldStorageCodec::NONE, *table},
            WorldStorageChunkInput{
                EWorldStorageChunkKind::WORLD_PARTITION_DATA,
                EWorldStorageCodec::NONE,
                wire.first(half)
            }
        };
        const WorldStorageChunkInput second{
            EWorldStorageChunkKind::WORLD_PARTITION_DATA,
            EWorldStorageCodec::NONE,
            wire.subspan(half)
        };
        auto a = encodeWorldStorageVolume(bundle, generation, 0, first);
        auto b = encodeWorldStorageVolume(bundle, generation, 1, std::span(&second, 1));
        assert(a && b);
        WorldDescriptionBuilder builder;
        assert(builder.setIdentity(bundle, generation, "pure/async parity"));
        assert(builder.addSchema(worldDataSchemaId("opaque.partition.test")));
        assert(builder.setPartitioner({worldPartitionerId("test.explicit"), 1}, 1));
        assert(builder.addStorageVolume({"a", 1, 2, a->size()}));
        assert(builder.addStorageVolume({"b", 1, 1, b->size()}));
        assert(builder.addPartitionTablePage({{0}, 1, {0, 0}}));
        auto description = std::move(builder).build();
        assert(description);
        auto own = [](std::vector<std::byte> bytes)
        {
            auto owner = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
            return cxx::SharedBytes<>::fromOwner(owner, *owner);
        };
        return {
            std::make_shared<const WorldDescription>(std::move(*description)),
            {own(std::move(*a)), own(std::move(*b))},
            std::move(*partition)
        };
    }

    void compare(
        Fixture source,
        std::size_t limit,
        std::optional<world::EWorldStorageCodecError> codec_error = {},
        std::optional<loading::EWorldStorageRuntimeError> runtime_error = {},
        bool cancel = false
    )
    {
        auto execution = execution::ExecutionRuntime::create({2, 64, {64}});
        assert(execution);
        std::stop_source stop;
        if (cancel)
        {
            stop.request_stop();
        }
        auto pure = world::decodeWorldStoragePartition(*source.world, source.volumes, {0}, limit, stop.get_token());
        auto memory = loading::makeWorldMemoryStorageSource(source.world, source.volumes);
        assert(memory);
        auto delayed = std::make_shared<Delayed>(std::move(*memory));
        auto transport = loading::WorldStorageSource::create(
            source.world,
            async::TOperationPort<loading::ReadWorldStorageRange>{delayed}
        );
        assert(transport);
        Result result;
        auto operation = stdexec::connect(
            loading::loadWorldPartition(*transport, execution->cpu(), {0}, limit, stop.get_token()),
            Receiver{result}
        );
        stdexec::start(operation);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!result.completions.load(std::memory_order_acquire))
        {
            assert(delayed->reads < 20 && std::chrono::steady_clock::now() < deadline);
            delayed->completeReady();
            std::this_thread::yield();
        }
        assert(result.completions == 1);
        if (cancel)
        {
            assert(!pure && pure.error().code == world::EWorldStorageCodecError::CANCELLED);
            assert(result.stopped && !result.data && !result.error && delayed->reads == 0);
        }
        else if (codec_error)
        {
            assert(!pure && pure.error().code == *codec_error);
            assert(result.error && result.error->code == *runtime_error);
            assert(pure.error().volume == result.error->volume && pure.error().offset == result.error->offset);
        }
        else
        {
            assert(pure && result.data && delayed->reads == 9);
            assert(result.thread != std::this_thread::get_id());
            auto a = world::encodeWorldPartitionData(*pure);
            auto b = world::encodeWorldPartitionData(*result.data);
            assert(a && b && *a == source.partition && *b == source.partition);
            assert(pure->id() == result.data->id());
        }
    }

    void ambientCancellation()
    {
        auto input = fixture();
        auto memory = loading::makeWorldMemoryStorageSource(input.world, input.volumes);
        assert(memory);
        auto delayed = std::make_shared<Delayed>(std::move(*memory));
        auto source = loading::WorldStorageSource::create(
            input.world,
            async::TOperationPort<loading::ReadWorldStorageRange>{delayed}
        );
        assert(source);
        auto runtime = execution::ExecutionRuntime::create({1, 16, {16}});
        assert(runtime);
        execution::TaskScope tasks{*runtime};
        std::atomic_bool cancelled{};
        assert(tasks.submit(
            {},
            [&](execution::TaskReporter) noexcept
            {
                auto loaded = stdexec::then(
                    loading::loadWorldPartition(*source, runtime->cpu(), {0}, 4096),
                    [](world::WorldPartitionData) noexcept -> execution::ETaskState { std::abort(); }
                );
                auto checked = stdexec::upon_error(
                    std::move(loaded),
                    [](loading::WorldStorageRuntimeFailure) noexcept -> execution::ETaskState { std::abort(); }
                );
                return stdexec::upon_stopped(
                    std::move(checked),
                    [&]() noexcept
                    {
                        cancelled.store(true, std::memory_order_release);
                        return execution::ETaskState::CANCELLED;
                    }
                );
            }
        ));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!delayed->pending())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        tasks.requestStop(); // Only the ambient std::stop_token; no explicit load token is supplied.
        delayed->completeReady();
        while (!tasks.settled())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        assert(cancelled.load(std::memory_order_acquire) && delayed->reads == 1);
    }

    void fileRanges(const std::filesystem::path& directory)
    {
        auto input = fixture();
        std::filesystem::create_directories(directory);
        for (std::size_t index{}; index < input.volumes.size(); ++index)
        {
            const auto path = directory / input.world->storageVolumes()[index].member_name;
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            const auto bytes = input.volumes[index].view();
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            assert(file);
        }
        auto runtime = execution::ExecutionRuntime::create({2, 32, {16}, execution::BlockingSchedulerConfig{1, 16}});
        assert(runtime);
        execution::TaskScope tasks{*runtime};
        auto files =
            loading::WorldFileStorageSource::create(input.world, directory, *runtime->blocking(), tasks, {1, 4096});
        assert(files);
        auto source = (*files)->source();
        const auto decoded = stdexec::sync_wait(loading::loadWorldPartition(source, runtime->cpu(), {0}, 4096));
        assert(decoded);
        auto encoded = world::encodeWorldPartitionData(std::get<0>(*decoded));
        assert(encoded && *encoded == input.partition);

        struct Reply final
        {
            std::optional<async::OperationOutcome<loading::ReadWorldStorageRange>> outcome;
            std::atomic_bool ready{};

            static void complete(
                void* state,
                async::OperationOutcome<loading::ReadWorldStorageRange>&& outcome
            ) noexcept
            {
                auto& reply = *static_cast<Reply*>(state);
                reply.outcome.emplace(std::move(outcome));
                reply.ready.store(true, std::memory_order_release);
            }

            void wait() const
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!ready.load(std::memory_order_acquire))
                {
                    assert(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::yield(); // Deliberately no Main dispatch.
                }
            }
        };

        Reply invalid;
        assert(source.readPort().submit({7, 0, 1}, &invalid, &Reply::complete, {}));
        invalid.wait();
        assert(invalid.outcome->error().domainError().code == loading::EWorldStorageRuntimeError::INVALID_VOLUME);
        Reply overflow;
        assert(source.readPort().submit({0, UINT64_MAX, 64}, &overflow, &Reply::complete, {}));
        overflow.wait();
        assert(overflow.outcome->error().domainError().code == loading::EWorldStorageRuntimeError::RANGE_OVERFLOW);

        // A short on-disk file is IO failure, even when the immutable descriptor still declares the full range.
        std::filesystem::resize_file(directory / "a", 4);
        Reply short_read;
        assert(source.readPort().submit({0, 0, 64}, &short_read, &Reply::complete, {}));
        short_read.wait();
        assert(short_read.outcome->error().domainError().code == loading::EWorldStorageRuntimeError::IO_FAILURE);

        std::binary_semaphore entered{0}, resume{0};
        assert(tasks.submit(
            {},
            [&](execution::TaskReporter) noexcept
            {
                return stdexec::schedule(*runtime->blocking()) | stdexec::then(
                                                                     [&]() noexcept
                                                                     {
                                                                         entered.release();
                                                                         resume.acquire();
                                                                     }
                                                                 );
            }
        ));
        entered.acquire();
        Reply accepted, rejected;
        assert(source.readPort().submit({1, 0, 64}, &accepted, &Reply::complete, {}));
        const auto full = source.readPort().submit({1, 0, 64}, &rejected, &Reply::complete, {});
        assert(!full && full.error() == async::ESubmitError::QUEUE_FULL);
        files->reset();
        const auto closed = source.readPort().submit({1, 0, 64}, &rejected, &Reply::complete, {});
        assert(!closed && closed.error() == async::ESubmitError::STOPPING);
        resume.release();
        accepted.wait();
        assert(*accepted.outcome && accepted.outcome->value().size() == 64 && !rejected.ready.load());
        assert(tasks.join());
    }
} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    using C = world::EWorldStorageCodecError;
    using R = loading::EWorldStorageRuntimeError;
    compare(fixture(), 4096);
    compare(fixture(), 0, C::SIZE_LIMIT, R::LIMIT_EXCEEDED);
    compare(fixture(), 80, C::SIZE_LIMIT, R::LIMIT_EXCEEDED);
    compare(fixture(), 4096, {}, {}, true);
    compare(fixture(true), 4096, C::CORRUPT_DESCRIPTOR, R::CORRUPT_DESCRIPTOR);
    auto missing = fixture();
    missing.volumes.pop_back();
    compare(std::move(missing), 4096, C::INVALID_VOLUME, R::INVALID_VOLUME);
    auto truncated = fixture();
    truncated.volumes[1] = truncated.volumes[1].subspan(0, 8);
    compare(std::move(truncated), 4096, C::RANGE_OVERFLOW, R::RANGE_OVERFLOW);
    auto corrupt = fixture();
    auto bytes =
        std::make_shared<std::vector<std::byte>>(corrupt.volumes[1].view().begin(), corrupt.volumes[1].view().end());
    bytes->back() ^= std::byte{1};
    corrupt.volumes[1] = cxx::SharedBytes<>::fromOwner(bytes, *bytes);
    compare(std::move(corrupt), 4096, C::DIGEST_MISMATCH, R::DIGEST_MISMATCH);
    fileRanges(argv[1]);
    ambientCancellation();
    std::cout
        << "Pure/async partition parity: multi-volume, extents, opaque payload, limits, cancellation, corruption OK\n";
}
