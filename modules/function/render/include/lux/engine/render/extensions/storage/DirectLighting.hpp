#pragma once

#include <lux/engine/render/extensions/storage/StorageDescriptor.hpp>
#include <lux/engine/render/inputs/DirectLightInputs.hpp>

namespace lux::render
{
    inline constexpr StorageNameView kDirectLightingStorageType{"lux.render.storage.DirectLighting"};

    struct DirectLightingBinding final
    {
        EDirectLightingBuffer field;
        bool operator==(const DirectLightingBinding&) const noexcept = default;
    };

    struct DirectLightingStorageDescriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const StorageDescriptor descriptor{
            kDirectLightingStorageType,
            0,
            0,
            cxx::typeToken<DirectLightingBinding>(),
            "Scene direct-light GPU arrays: directional, finite point and spot lights, with independent FIF replicas."
        };
    };
} // namespace lux::render
