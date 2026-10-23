#include <lux/engine/core/execution/PortSender.hpp>
#include <lux/engine/process/world_loading/WorldFileStorageSource.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <mutex>

namespace lux::process::world_loading
{
    namespace
    {
        using Port = lux::async::TOperationPort<ReadWorldStorageRange>;
        using Failure = lux::async::TOperationFailure<WorldStorageRuntimeFailure>;
        using ReadResult = lux::cxx::expected<lux::cxx::SharedBytes<>, WorldStorageRuntimeFailure>;

        [[nodiscard]] ReadResult readRange(const std::filesystem::path& path, ReadWorldStorageRange range) noexcept
        {
            std::ifstream file(path, std::ios::binary);
            file.seekg(static_cast<std::streamoff>(range.offset));
            if (!file)
            {
                return lux::cxx::unexpected(
                    WorldStorageRuntimeFailure{EWorldStorageRuntimeError::IO_FAILURE, range.volume, range.offset}
                );
            }
            auto bytes = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(range.size));
            if (!file.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(range.size)))
            {
                return lux::cxx::unexpected(
                    WorldStorageRuntimeFailure{EWorldStorageRuntimeError::IO_FAILURE, range.volume, range.offset}
                );
            }
            return lux::cxx::SharedBytes<>::fromOwner(bytes, *bytes);
        }

        [[nodiscard]] lux::async::ESubmitError admissionError(execution::EExecutionError error) noexcept
        {
            if (error == execution::EExecutionError::CAPACITY_EXCEEDED)
            {
                return lux::async::ESubmitError::QUEUE_FULL;
            }
            const bool stopping =
                error == execution::EExecutionError::STOPPING || error == execution::EExecutionError::ALREADY_JOINED;
            return stopping ? lux::async::ESubmitError::STOPPING : lux::async::ESubmitError::UNKNOWN_OPERATION;
        }
    } // namespace

    struct WorldFileStorageSource::Impl final : Port::Endpoint, std::enable_shared_from_this<Impl>
    {
        struct Request final
        {
            std::shared_ptr<Impl> files;
            void* state;
            void (*complete)(void*, Outcome&&) noexcept;

            void finish(Outcome outcome) const noexcept
            {
                {
                    std::lock_guard lock{files->mutex};
                    --files->admitted;
                }
                // File access has ended. The continuation may immediately request its next range.
                complete(state, std::move(outcome));
            }
        };

        Impl(
            std::shared_ptr<const world::WorldDescription> description,
            std::vector<std::filesystem::path> files,
            execution::BlockingScheduler scheduler,
            execution::ExecutionRuntime& execution,
            WorldFileStorageLimits limits,
            std::stop_token stop
        ) noexcept
            : world(std::move(description)), paths(std::move(files)), blocking(scheduler), execution(&execution),
              limits(limits), stop(stop)
        {
        }

        void close() noexcept
        {
            std::lock_guard lock{mutex};
            execution = nullptr;
        }

        lux::async::SubmitResult submit(
            ReadWorldStorageRange range,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions
        ) noexcept override
        {
            if (range.volume >= paths.size())
            {
                complete(
                    state,
                    lux::cxx::unexpected(
                        Failure::domain({EWorldStorageRuntimeError::INVALID_VOLUME, range.volume, range.offset})
                    )
                );
                return {};
            }
            const auto volume_bytes = world->storageVolumes()[range.volume].file_size;
            const bool valid_offset =
                range.offset <= volume_bytes &&
                range.offset <= static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max());
            const bool valid_range = valid_offset && range.size <= volume_bytes - range.offset;
            const bool valid_size =
                range.size <= limits.range_bytes &&
                range.size <= static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max());
            if (!valid_range || !valid_size)
            {
                const auto error =
                    valid_range ? EWorldStorageRuntimeError::LIMIT_EXCEEDED : EWorldStorageRuntimeError::RANGE_OVERFLOW;
                complete(state, lux::cxx::unexpected(Failure::domain({error, range.volume, range.offset})));
                return {};
            }
            execution::ExecutionRuntime* accepted;
            {
                std::lock_guard lock{mutex};
                if (!execution || stop.stop_requested())
                {
                    return lux::cxx::unexpected(lux::async::ESubmitError::STOPPING);
                }
                if (admitted == limits.requests)
                {
                    return lux::cxx::unexpected(lux::async::ESubmitError::QUEUE_FULL);
                }
                accepted = execution;
                ++admitted;
            }
            Request request{shared_from_this(), state, complete};
            auto bytes = stdexec::then(
                stdexec::schedule(blocking),
                [files = request.files, range]() noexcept { return readRange(files->paths[range.volume], range); }
            );
            auto delivered = stdexec::then(
                execution::taskResult(std::move(bytes)),
                [request = std::move(request)](auto result) noexcept
                {
                    if (result)
                    {
                        request.finish(Outcome{std::move(*result)});
                    }
                    else if (const auto* failure = result.error().domainFailure())
                    {
                        request.finish(lux::cxx::unexpected(Failure::domain(*failure)));
                    }
                    else
                    {
                        const auto* execution_error = result.error().executionFailure();
                        const auto error =
                            execution_error ? admissionError(*execution_error) : lux::async::ESubmitError::STOPPING;
                        request.finish(lux::cxx::unexpected(Failure::runtime(error)));
                    }
                }
            );
            const auto submitted =
                accepted->spawn(stdexec::write_env(std::move(delivered), stdexec::prop{stdexec::get_stop_token, stop}));
            if (!submitted)
            {
                std::lock_guard lock{mutex};
                --admitted;
                return lux::cxx::unexpected(admissionError(submitted.error()));
            }
            return {};
        }

        std::shared_ptr<const world::WorldDescription> world;
        std::vector<std::filesystem::path> paths;
        execution::BlockingScheduler blocking;
        std::mutex mutex;
        execution::ExecutionRuntime* execution;
        std::stop_token stop;
        WorldFileStorageLimits limits;
        std::size_t admitted{};
    };

    WorldFileStorageSource::WorldFileStorageSource(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    WorldFileStorageSource::~WorldFileStorageSource() noexcept
    {
        impl_->close();
    }

    WorldFileStorageSource::CreateResult WorldFileStorageSource::create(
        std::shared_ptr<const world::WorldDescription> world,
        std::filesystem::path directory,
        execution::BlockingScheduler blocking,
        execution::ExecutionRuntime& execution,
        WorldFileStorageLimits limits,
        std::stop_token stop
    ) noexcept
    {
        const bool invalid_inputs = !world || !blocking || directory.empty();
        const bool invalid_limits = limits.requests == 0 || limits.range_bytes == 0;
        if (invalid_inputs || invalid_limits)
        {
            return lux::cxx::unexpected(WorldStorageRuntimeFailure{EWorldStorageRuntimeError::INVALID_SOURCE});
        }
        std::vector<std::filesystem::path> files;
        files.reserve(world->storageVolumes().size());
        for (const auto& volume : world->storageVolumes())
        {
            const auto name = std::filesystem::u8path(volume.member_name);
            const bool escapes_directory =
                name.has_root_path() ||
                std::ranges::any_of(name, [](const auto& part) noexcept { return part == ".."; });
            if (name.empty() || escapes_directory)
            {
                return lux::cxx::unexpected(WorldStorageRuntimeFailure{EWorldStorageRuntimeError::INVALID_SOURCE});
            }
            files.push_back(directory / name);
        }
        auto impl = std::make_shared<Impl>(std::move(world), std::move(files), blocking, execution, limits, stop);
        return std::unique_ptr<WorldFileStorageSource>{new WorldFileStorageSource(std::move(impl))};
    }

    WorldStorageSource WorldFileStorageSource::source() const noexcept
    {
        return *WorldStorageSource::create(impl_->world, Port{impl_});
    }
} // namespace lux::process::world_loading
