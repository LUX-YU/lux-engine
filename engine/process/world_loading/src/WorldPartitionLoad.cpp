#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>
#include <lux/engine/world/storage/detail/WorldPartitionDecoder.hpp>
#include <lux/engine/process/PortSender.hpp>

#include <atomic>
#include <limits>
#include <optional>
#include <utility>

namespace lux::process::world_loading
{
    namespace
    {
        [[nodiscard]] WorldStorageRuntimeFailure sourceFailure() noexcept
        {
            return WorldStorageRuntimeFailure{EWorldStorageRuntimeError::INVALID_SOURCE};
        }
    } // namespace

    WorldStorageSource::WorldStorageSource(
        std::shared_ptr<const lux::world::WorldDescription> world,
        lux::async::TOperationPort<ReadWorldStorageRange> read_port
    ) noexcept
        : world_(std::move(world)), read_port_(std::move(read_port))
    {}

    lux::cxx::expected<WorldStorageSource, WorldStorageRuntimeFailure> WorldStorageSource::create(
        std::shared_ptr<const lux::world::WorldDescription> world,
        lux::async::TOperationPort<ReadWorldStorageRange> read_port
    ) noexcept
    {
        if (!world || !read_port)
            return lux::cxx::unexpected(sourceFailure());
        return WorldStorageSource(std::move(world), std::move(read_port));
    }

    WorldStorageSource::operator bool() const noexcept
    {
        return world_ != nullptr && static_cast<bool>(read_port_);
    }

    const lux::world::WorldDescription& WorldStorageSource::world() const noexcept
    {
        return *world_;
    }

    const lux::async::TOperationPort<ReadWorldStorageRange>& WorldStorageSource::readPort() const noexcept
    {
        return read_port_;
    }

    struct detail::WorldPartitionLoadMachine::Impl final
    {
        using Outcome = lux::async::OperationOutcome<ReadWorldStorageRange>;

        Impl(
            WorldStorageSource source_value,
            partition::PartitionOrdinal partition_value,
            std::size_t max_bytes_value,
            std::stop_token stop_value,
            process::TaskReporter reporter,
            void* receiver_value,
            void (*value_fn)(void*, lux::world::WorldPartitionData&&) noexcept,
            void (*error_fn)(void*, WorldStorageRuntimeFailure) noexcept,
            void (*stopped_fn)(void*) noexcept
        ) noexcept
            : source(std::move(source_value)), partition(partition_value), max_bytes(max_bytes_value), stop(stop_value),
              correlation(taskCorrelation(reporter.id())), receiver(receiver_value), set_value(value_fn),
              set_error(error_fn), set_stopped(stopped_fn)
        {}

        [[nodiscard]] WorldStorageRuntimeFailure mapFailure(lux::world::WorldStorageCodecFailure failure) const noexcept
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

        void finishError(WorldStorageRuntimeFailure failure) noexcept
        {
            if (!finished.exchange(true, std::memory_order_acq_rel))
                set_error(receiver, failure);
        }

        void finishStopped() noexcept
        {
            if (!finished.exchange(true, std::memory_order_acq_rel))
                set_stopped(receiver);
        }

        void finishCodecFailure(lux::world::WorldStorageCodecFailure failure) noexcept
        {
            if (failure.code == lux::world::EWorldStorageCodecError::CANCELLED)
                finishStopped();
            else
                finishError(mapFailure(failure));
        }

        void submit(std::uint32_t volume, std::uint64_t offset, std::uint64_t size) noexcept
        {
            if (stop.stop_requested())
            {
                finishStopped();
                return;
            }
            if (size > std::numeric_limits<std::size_t>::max() || static_cast<std::size_t>(size) > max_bytes)
            {
                finishError({EWorldStorageRuntimeError::LIMIT_EXCEEDED, volume, offset});
                return;
            }
            const std::size_t accounted_bytes = static_cast<std::size_t>(size);
            callback_seen.store(false, std::memory_order_release);
            const auto submitted = source.readPort().submit(
                ReadWorldStorageRange{volume, offset, size},
                this,
                [](void* state, Outcome&& outcome) noexcept {
                    auto& self = *static_cast<Impl*>(state);
                    self.callback_seen.store(true, std::memory_order_release);
                    self.complete(std::move(outcome));
                },
                lux::async::SubmitOptions{.accounted_bytes = accounted_bytes, .correlation = correlation}
            );
            if (!submitted && !callback_seen.load(std::memory_order_acquire))
            {
                finishError({EWorldStorageRuntimeError::IO_FAILURE, volume, offset});
            }
        }

        void advance(lux::world::detail::WorldPartitionDecoder::Next next) noexcept
        {
            if (!next)
            {
                finishCodecFailure(next.error());
                return;
            }
            if (*next)
            {
                current_range = **next;
                submit(current_range.volume, current_range.offset, current_range.size);
                return;
            }
            if (!finished.exchange(true, std::memory_order_acq_rel))
                set_value(receiver, std::move(*decoder).takeResult());
        }

        void complete(Outcome&& outcome) noexcept
        {
            if (finished.load(std::memory_order_acquire))
                return;
            if (stop.stop_requested())
            {
                finishStopped();
                return;
            }
            if (!outcome)
            {
                if (outcome.error().isRuntime())
                    finishError({EWorldStorageRuntimeError::IO_FAILURE, current_range.volume});
                else
                    finishError(outcome.error().domainError());
                return;
            }
            advance(decoder->accept(outcome->view()));
        }

        void start() noexcept
        {
            if (!source)
            {
                finishError({max_bytes == 0U ? EWorldStorageRuntimeError::LIMIT_EXCEEDED
                                            : EWorldStorageRuntimeError::INVALID_PARTITION});
                return;
            }
            decoder.emplace(source.world(), partition, max_bytes, stop);
            advance(decoder->start());
        }

        WorldStorageSource source;
        partition::PartitionOrdinal partition;
        std::size_t max_bytes{};
        std::stop_token stop;
        std::array<std::uint64_t, 2> correlation{};
        void* receiver{};
        void (*set_value)(void*, lux::world::WorldPartitionData&&) noexcept {};
        void (*set_error)(void*, WorldStorageRuntimeFailure) noexcept {};
        void (*set_stopped)(void*) noexcept {};
        std::atomic_bool finished{};
        std::atomic_bool callback_seen{};
        std::optional<lux::world::detail::WorldPartitionDecoder> decoder;
        lux::world::detail::WorldStorageReadRange current_range;

    };

    detail::WorldPartitionLoadMachine::WorldPartitionLoadMachine(
        WorldStorageSource source,
        partition::PartitionOrdinal partition,
        std::size_t max_bytes,
        std::stop_token stop,
        process::TaskReporter reporter,
        void* receiver,
        void (*set_value)(void*, lux::world::WorldPartitionData&&) noexcept,
        void (*set_error)(void*, WorldStorageRuntimeFailure) noexcept,
        void (*set_stopped)(void*) noexcept
    )
        : impl_(std::make_unique<Impl>(
              std::move(source),
              partition,
              max_bytes,
              stop,
              reporter,
              receiver,
              set_value,
              set_error,
              set_stopped
          ))
    {}

    detail::WorldPartitionLoadMachine::~WorldPartitionLoadMachine() = default;

    void detail::WorldPartitionLoadMachine::start() noexcept
    {
        impl_->start();
    }
} // namespace lux::scene
