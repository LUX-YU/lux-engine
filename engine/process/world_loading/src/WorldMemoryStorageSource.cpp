#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <vector>

namespace lux::process::world_loading
{
    namespace
    {
        class MemoryVolumes final : public lux::async::TOperationPort<ReadWorldStorageRange>::Endpoint
        {
        public:
            explicit MemoryVolumes(std::span<const lux::cxx::SharedBytes<>> volumes)
                : volumes_(volumes.begin(), volumes.end())
            {}

            lux::async::SubmitResult submit(
                ReadWorldStorageRange request,
                void* state,
                void (*complete)(void*, lux::async::OperationOutcome<ReadWorldStorageRange>&&) noexcept,
                lux::async::SubmitOptions
            ) noexcept override
            {
                using Failure = lux::async::TOperationFailure<WorldStorageRuntimeFailure>;
                if (request.volume >= volumes_.size())
                {
                    complete(
                        state,
                        lux::cxx::unexpected(
                            Failure::domain({EWorldStorageRuntimeError::INVALID_VOLUME, request.volume, request.offset})
                        )
                    );
                    return {};
                }
                const auto& bytes = volumes_[request.volume];
                if (request.offset > bytes.size() || request.size > bytes.size() - request.offset)
                {
                    complete(
                        state,
                        lux::cxx::unexpected(
                            Failure::domain({EWorldStorageRuntimeError::RANGE_OVERFLOW, request.volume, request.offset})
                        )
                    );
                    return {};
                }
                complete(
                    state,
                    bytes.subspan(static_cast<std::size_t>(request.offset), static_cast<std::size_t>(request.size))
                );
                return {};
            }

        private:
            std::vector<lux::cxx::SharedBytes<>> volumes_;
        };

    }

    lux::cxx::expected<WorldStorageSource, WorldStorageRuntimeFailure> makeWorldMemoryStorageSource(
        std::shared_ptr<const world::WorldDescription> world,
        std::span<const lux::cxx::SharedBytes<>> volumes
    ) noexcept
    {
        return WorldStorageSource::create(
            std::move(world),
            lux::async::TOperationPort<ReadWorldStorageRange>{std::make_shared<MemoryVolumes>(volumes)}
        );
    }
}
