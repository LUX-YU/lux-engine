#include <lux/engine/domain/WorldObjectId.hpp>
#include <lux/engine/process/world/WorldPartitionLoadSender.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/storage/detail/WorldStorageCodec.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexec/execution.hpp>
#include <utility>
#include <vector>

namespace
{
    template <class Type>
    [[nodiscard]] Type id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = tail;
        return Type{uuids::uuid(bytes)};
    }

    class MemoryEndpoint final
        : public lux::async::OperationPort<lux::process::world::ReadWorldStorageRange>::Endpoint
    {
    public:
        explicit MemoryEndpoint(std::vector<std::byte> bytes) : bytes_(std::move(bytes))
        {
        }

        [[nodiscard]] lux::async::SubmitResult submit(
            lux::process::world::ReadWorldStorageRange operation,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions options
        ) noexcept override
        {
            accounting_ok = accounting_ok && options.accounted_bytes == operation.size;
            if (operation.volume != 0U || operation.offset > bytes_.size() ||
                operation.size > bytes_.size() - operation.offset)
            {
                complete(
                    state,
                    lux::cxx::unexpected(
                        lux::async::OperationFailure<lux::process::world::WorldStorageRuntimeFailure>::domain(
                            {lux::process::world::EWorldStorageRuntimeError::RANGE_OVERFLOW}
                        )
                    )
                );
                return {};
            }
            complete(
                state,
                Outcome{lux::cxx::SharedBytes<>::copyOf(
                    std::span<const std::byte>(bytes_).subspan(
                        static_cast<std::size_t>(operation.offset),
                        static_cast<std::size_t>(operation.size)
                    )
                )}
            );
            return {};
        }

        bool accounting_ok{true};

    private:
        std::vector<std::byte> bytes_;
    };

    struct Receiver final
    {
        using receiver_concept = stdexec::receiver_t;

        void set_value(lux::world::WorldPartitionData value) && noexcept
        {
            result->emplace(std::move(value));
        }

        void set_error(lux::process::world::WorldStorageRuntimeFailure value) && noexcept
        {
            error->emplace(value);
        }

        void set_stopped() && noexcept
        {
            *stopped = true;
        }

        [[nodiscard]] stdexec::empty_env get_env() const noexcept
        {
            return {};
        }

        std::optional<lux::world::WorldPartitionData>* result{};
        std::optional<lux::process::world::WorldStorageRuntimeFailure>* error{};
        bool* stopped{};
    };
}

int main()
{
    using namespace lux::world;
    using namespace lux::world::detail;

    assert(!lux::process::world::WorldStorageSource::create({}, {}));

    const std::array objects{
        WorldEncodedObjectRecord{id<lux::domain::WorldObjectId>(1U), {}}
    };
    auto partition = encodeWorldPartitionData(lux::partition::PartitionOrdinal{0U}, objects);
    assert(partition);

    const std::array records{WorldPartitionRecord{id<WorldPartitionId>(2U), 0U, 1U}};
    const std::array extents{WorldPartitionExtent{0U, 1U, 1U}};
    auto page = encodeWorldPartitionTablePage(lux::partition::PartitionOrdinal{0U}, records, extents);
    assert(page);

    const std::array chunks{
        WorldStorageChunkInput{EWorldStorageChunkKind::PARTITION_TABLE_PAGE, EWorldStorageCodec::NONE, *page},
        WorldStorageChunkInput{EWorldStorageChunkKind::WORLD_PARTITION_DATA, EWorldStorageCodec::NONE, *partition}
    };
    const auto bundle = id<WorldBundleId>(3U);
    const auto generation = id<WorldBundleGeneration>(4U);
    auto volume = encodeWorldStorageVolume(bundle, generation, 0U, chunks);
    assert(volume);

    WorldDescriptionBuilder builder;
    assert(builder.setIdentity(bundle, generation, "process-world"));
    assert(builder.setPartitioner({worldPartitionerId("process.world.test"), 1U}, 1U));
    assert(builder.addStorageVolume({"process.wvol0", 1U, 2U, volume->size()}));
    assert(builder.addPartitionTablePage({lux::partition::PartitionOrdinal{0U}, 1U, {0U, 0U}}));
    auto description = std::move(builder).build();
    assert(description);

    auto endpoint = std::make_shared<MemoryEndpoint>(*volume);
    auto source = lux::process::world::WorldStorageSource::create(
        std::make_shared<WorldDescription>(std::move(*description)),
        lux::async::OperationPort<lux::process::world::ReadWorldStorageRange>{endpoint}
    );
    assert(source);

    std::optional<WorldPartitionData> result;
    std::optional<lux::process::world::WorldStorageRuntimeFailure> error;
    bool stopped{};
    auto state = stdexec::connect(
        lux::process::world::loadWorldPartition(
            *source,
            lux::partition::PartitionOrdinal{0U},
            volume->size(),
            {}
        ),
        Receiver{&result, &error, &stopped}
    );
    stdexec::start(state);

    assert(result);
    assert(result->objectCount() == 1U);
    assert(!error);
    assert(!stopped);
    assert(endpoint->accounting_ok);
    return 0;
}
