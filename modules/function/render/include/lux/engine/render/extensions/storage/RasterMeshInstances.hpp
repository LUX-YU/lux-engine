#pragma once

#include <lux/engine/render/extensions/storage/StorageDescriptor.hpp>
#include <lux/engine/render/inputs/RasterInstanceInputs.hpp>

namespace lux::render
{
    inline constexpr StorageNameView kMeshInstanceStorageType("lux.render.storage.RasterMeshInstances");

    enum class EMeshInstanceBinding : std::uint8_t
    {
        TRANSFORMS,
        PROPERTIES,
        ALIVE
    };

    struct MeshInstanceBinding final
    {
        EMeshInstanceBinding role = EMeshInstanceBinding::TRANSFORMS;
        bool operator==(const MeshInstanceBinding&) const noexcept = default;
    };

    class RasterMeshInstanceStorageDescriptor final
    {
    public:
        using Config = void;
        static inline const StorageDescriptor descriptor{
            kMeshInstanceStorageType,
            0,
            0,
            cxx::typeToken<MeshInstanceBinding>(),
            "Paged raster transforms, indexed mesh/material references and live records; no skinning ABI."
        };

        using Ops = void;
    };
} // namespace lux::render
