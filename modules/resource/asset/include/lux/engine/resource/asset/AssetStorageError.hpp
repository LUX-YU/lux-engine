#pragma once

#include <cstdint>

namespace lux::asset
{
    enum class EAssetStorageError : std::uint8_t
    {
        NOT_FOUND,
        IO_FAILURE,
        CORRUPT_IMAGE,
        UNSUPPORTED,
        LIMIT_EXCEEDED,
    };
} // namespace lux::asset
