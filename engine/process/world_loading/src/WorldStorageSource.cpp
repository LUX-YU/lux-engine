#include <lux/engine/process/world_loading/WorldStorageSource.hpp>

namespace lux::process::world_loading
{
    WorldStorageSource::WorldStorageSource(
        std::shared_ptr<const lux::world::WorldDescription> world,
        lux::async::TOperationPort<ReadWorldStorageRange> read_port
    ) noexcept
        : world_(std::move(world)), read_port_(std::move(read_port))
    {
    }

    lux::cxx::expected<WorldStorageSource, WorldStorageRuntimeFailure> WorldStorageSource::create(
        std::shared_ptr<const lux::world::WorldDescription> world,
        lux::async::TOperationPort<ReadWorldStorageRange> read_port
    ) noexcept
    {
        if (!world || !read_port)
        {
            return lux::cxx::unexpected(WorldStorageRuntimeFailure{EWorldStorageRuntimeError::INVALID_SOURCE});
        }
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

} // namespace lux::process::world_loading
