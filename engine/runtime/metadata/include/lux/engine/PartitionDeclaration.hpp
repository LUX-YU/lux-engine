#pragma once

#include <lux/engine/error/Error.hpp>
#include <lux/engine/scene/SceneStreamingData.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace lux::world
{
    // Cold scene-content compilation contract. No world instances, IO, or asset ownership.
    struct PartitionDeclaration final
    {
        std::string name;
        std::uint32_t configuration_version{};
        serialization::PortableValueCodec configuration;
        scene::ESceneSpatialDimension dimension{};
        cxx::expected<scene::SceneSpatialIndex, error::Error> (*build_index)(
            std::span<const scene::SceneSpatialEntry>, std::span<const std::byte>
        ) noexcept {};
    };
} // namespace lux::world
