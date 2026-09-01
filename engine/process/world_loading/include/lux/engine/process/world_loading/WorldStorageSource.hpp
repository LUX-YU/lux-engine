#pragma once

#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/process/world_loading/visibility.h>
#include <lux/engine/world/WorldDescription.hpp>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstdint>
#include <memory>

namespace lux::process::world_loading
{
    enum class EWorldStorageRuntimeError : std::uint8_t
    {
        INVALID_SOURCE,
        INVALID_PARTITION,
        INVALID_VOLUME,
        BUNDLE_MISMATCH,
        RANGE_OVERFLOW,
        LIMIT_EXCEEDED,
        IO_FAILURE,
        CORRUPT_DESCRIPTOR,
        DIGEST_MISMATCH,
        DECOMPRESSION_FAILURE,
        DECODE_FAILURE,
        ALLOCATION_FAILURE,
    };

    struct WorldStorageRuntimeFailure final
    {
        EWorldStorageRuntimeError code{EWorldStorageRuntimeError::INVALID_SOURCE};
        std::uint32_t volume{};
        std::uint64_t offset{};
    };

    struct ReadWorldStorageRange final
    {
        using Value = lux::cxx::SharedBytes<>;
        using Error = WorldStorageRuntimeFailure;

        std::uint32_t volume{};
        std::uint64_t offset{};
        std::uint64_t size{};
    };

    class LUX_ENGINE_PROCESS_WORLD_LOADING_PUBLIC WorldStorageSource final
    {
    public:
        WorldStorageSource() noexcept = default;

        [[nodiscard]] static lux::cxx::expected<WorldStorageSource, WorldStorageRuntimeFailure> create(
            std::shared_ptr<const lux::world::WorldDescription> world,
            lux::async::OperationPort<ReadWorldStorageRange> read_port
        ) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] const lux::world::WorldDescription& world() const noexcept;
        [[nodiscard]] const lux::async::OperationPort<ReadWorldStorageRange>& readPort() const noexcept;

    private:
        WorldStorageSource(
            std::shared_ptr<const lux::world::WorldDescription> world,
            lux::async::OperationPort<ReadWorldStorageRange> read_port
        ) noexcept;

        std::shared_ptr<const lux::world::WorldDescription> world_;
        lux::async::OperationPort<ReadWorldStorageRange> read_port_;
    };
}
