#pragma once

#include <lux/engine/scene/SceneStreamingData.hpp>
#include <lux/engine/world/storage/visibility.h>
#include <lux/engine/error/Error.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <span>

namespace lux::serialization
{
    class BinaryWriter;
    class BinaryReader;
}

namespace lux::scene
{
    // Nested Scene-root fields. remaining_bytes is the shared wire/allocation budget of this codec call.
    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC cxx::expected<void, error::Error>
    writeSceneStreaming(
        serialization::BinaryWriter& writer, const SceneStreamingData& data, std::size_t& remaining_bytes
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC cxx::expected<SceneStreamingData, error::Error>
    readSceneStreaming(serialization::BinaryReader& reader, std::size_t& remaining_bytes) noexcept;
} // namespace lux::scene
