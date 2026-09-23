#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/partition/PartitionOrdinal.hpp>
#include <lux/engine/scene/world_loading/visibility.h>
#include <lux/engine/serialization/PortableValueCodec.hpp>

#include <vector>

namespace lux::scene
{
struct LUX_TYPE_INFO(both) WorldLoadingConfiguration final
{
    std::vector<partition::PartitionOrdinal> LUX_MEMBER(display_name = BootstrapPartitions) bootstrap;
};

[[nodiscard]] LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC serialization::PortableValueCodec
worldLoadingConfigurationCodec() noexcept;
} // namespace lux::scene
