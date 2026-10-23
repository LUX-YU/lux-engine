#include <lux/engine/core/execution/PortSender.hpp>
#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>
#include <lux/engine/world/storage/detail/WorldPartitionDecoder.hpp>

#include <limits>

namespace lux::process::world_loading::detail
{
    namespace
    {
        [[nodiscard]] WorldStorageRuntimeFailure mapCodecFailure(world::WorldStorageCodecFailure failure) noexcept
        {
            using Input = lux::world::EWorldStorageCodecError;
            EWorldStorageRuntimeError code{EWorldStorageRuntimeError::DECODE_FAILURE};
            switch (failure.code)
            {
            case Input::INVALID_PARTITION:
                code = EWorldStorageRuntimeError::INVALID_PARTITION;
                break;
            case Input::INVALID_VOLUME:
                code = EWorldStorageRuntimeError::INVALID_VOLUME;
                break;
            case Input::BUNDLE_MISMATCH:
            case Input::GENERATION_MISMATCH:
            case Input::VOLUME_MISMATCH:
                code = EWorldStorageRuntimeError::BUNDLE_MISMATCH;
                break;
            case Input::RANGE_OVERFLOW:
                code = EWorldStorageRuntimeError::RANGE_OVERFLOW;
                break;
            case Input::SIZE_LIMIT:
                code = EWorldStorageRuntimeError::LIMIT_EXCEEDED;
                break;
            case Input::DIGEST_MISMATCH:
                code = EWorldStorageRuntimeError::DIGEST_MISMATCH;
                break;
            case Input::UNSUPPORTED_CODEC:
                code = EWorldStorageRuntimeError::DECOMPRESSION_FAILURE;
                break;
            case Input::ALLOCATION_FAILURE:
                code = EWorldStorageRuntimeError::ALLOCATION_FAILURE;
                break;
            case Input::CORRUPT_DESCRIPTOR:
            case Input::INVALID_MAGIC:
            case Input::UNSUPPORTED_VERSION:
                code = EWorldStorageRuntimeError::CORRUPT_DESCRIPTOR;
                break;
            default:
                break;
            }
            return WorldStorageRuntimeFailure{code, failure.volume, failure.offset};
        }
    } // namespace

    PartitionLoadTask decodeWorldPartition(
        WorldStorageSource source,
        partition::PartitionOrdinal partition,
        std::size_t max_bytes,
        std::stop_token explicit_stop,
        execution::TaskReporter reporter
    ) noexcept
    {
        const auto ambient_stop = co_await stdexec::read_env(stdexec::get_stop_token);
        std::stop_source cancellation;
        const auto request_stop = [&cancellation]() noexcept { cancellation.request_stop(); };
        std::stop_callback explicit_callback{explicit_stop, request_stop};
        stdexec::stop_callback_for_t<decltype(ambient_stop), decltype(request_stop)> ambient_callback{
            ambient_stop,
            request_stop
        };
        const auto stop = cancellation.get_token();
        if (stop.stop_requested())
        {
            co_await stdexec::just_stopped();
        }
        if (!source)
        {
            co_return lux::cxx::unexpected(WorldStorageRuntimeFailure{EWorldStorageRuntimeError::INVALID_SOURCE});
        }
        world::detail::WorldPartitionDecoder decoder{source.world(), partition, max_bytes, stop};
        auto next = decoder.start();
        while (next && *next)
        {
            if (stop.stop_requested())
            {
                co_await stdexec::just_stopped();
            }
            const auto range = **next;
            const bool exceeds_address_space = range.size > std::numeric_limits<std::size_t>::max();
            const bool exceeds_read_limit = range.size > max_bytes;
            if (exceeds_address_space || exceeds_read_limit)
            {
                co_return lux::cxx::unexpected(
                    WorldStorageRuntimeFailure{EWorldStorageRuntimeError::LIMIT_EXCEEDED, range.volume, range.offset}
                );
            }
            using Outcome = lux::async::OperationOutcome<ReadWorldStorageRange>;
            using Failure = lux::async::TOperationFailure<WorldStorageRuntimeFailure>;
            const lux::async::SubmitOptions options{
                .accounted_bytes = static_cast<std::size_t>(range.size),
                .correlation = execution::taskCorrelation(reporter.id())
            };
            auto read = stdexec::then(
                execution::portSender(
                    source.readPort(),
                    ReadWorldStorageRange{range.volume, range.offset, range.size},
                    options
                ),
                [](lux::cxx::SharedBytes<> bytes) noexcept -> Outcome { return bytes; }
            );
            auto outcome = co_await stdexec::upon_error(
                std::move(read),
                [](Failure failure) noexcept -> Outcome { return lux::cxx::unexpected(std::move(failure)); }
            );
            // exec::task inherits the CPU scheduler at starts_on. Every read returns there before
            // decoder.accept, including a synchronously completing memory provider.
            if (stop.stop_requested())
            {
                co_await stdexec::just_stopped();
            }
            if (!outcome)
            {
                const auto failure =
                    outcome.error().isRuntime()
                        ? WorldStorageRuntimeFailure{EWorldStorageRuntimeError::IO_FAILURE, range.volume, range.offset}
                        : outcome.error().domainError();
                co_return lux::cxx::unexpected(failure);
            }
            next = decoder.accept(outcome->view());
        }
        if (!next)
        {
            if (next.error().code == world::EWorldStorageCodecError::CANCELLED)
            {
                co_await stdexec::just_stopped();
            }
            co_return lux::cxx::unexpected(mapCodecFailure(next.error()));
        }
        co_return std::move(decoder).takeResult();
    }
} // namespace lux::process::world_loading::detail
