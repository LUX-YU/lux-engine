#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>
#include <array>
#include <cassert>
#include <iostream>
#include <optional>

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
        unsigned completions{};
        bool stopped{};
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
            ++result.completions;
            result.data = std::move(data);
        }
        void set_error(loading::WorldStorageRuntimeFailure error) && noexcept
        {
            ++result.completions;
            result.error = error;
        }
        void set_stopped() && noexcept
        {
            ++result.completions;
            result.stopped = true;
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
            assert(!pending_);
            pending_ = Request{range, state, complete, options};
            return {};
        }
        void complete()
        {
            assert(pending_);
            const auto request = *pending_;
            pending_.reset();
            assert(source_.readPort().submit(request.range, request.state, request.complete, request.options));
            ++reads;
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
        auto own = [](std::vector<std::byte> bytes) {
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
        std::stop_source stop;
        if (cancel)
            stop.request_stop();
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
        auto operation =
            stdexec::connect(loading::loadWorldPartition(*transport, {0}, limit, stop.get_token()), Receiver{result});
        stdexec::start(operation);
        while (!result.completions)
        {
            assert(delayed->reads < 20);
            delayed->complete();
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
            auto a = world::encodeWorldPartitionData(*pure);
            auto b = world::encodeWorldPartitionData(*result.data);
            assert(a && b && *a == source.partition && *b == source.partition);
            assert(pure->id() == result.data->id());
        }
    }
}

int main()
{
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
    std::cout
        << "Pure/async partition parity: multi-volume, extents, opaque payload, limits, cancellation, corruption OK\n";
}
