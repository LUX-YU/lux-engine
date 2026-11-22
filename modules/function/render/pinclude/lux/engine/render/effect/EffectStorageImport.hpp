#pragma once

#include <lux/engine/render/core/Resource.hpp>
#include <lux/engine/render/extensions/execution/StorageSnapshot.hpp>
#include <lux/engine/render/graph/StorageBindingSlot.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>

namespace lux::render::runtime_detail
{
    struct EffectStorageImport final
    {
        struct SnapshotContract final
        {
            cxx::basic_type_info::id_t hash;
            std::string name;
            ESnapshotScope scope;
            bool operator==(const SnapshotContract&) const noexcept = default;
        };

        enum class ERasterOwner : std::uint8_t
        {
            TEXTURES,
            MESHES,
            MATERIALS
        };

        std::string binding;
        ResourceName type;
        std::variant<detail::StorageGraphImport, SnapshotContract, ERasterOwner> contract;
        std::uint32_t projection;
        bool optional = false;
        std::uint32_t graph_uses = 0;
        detail::StorageBindingSlot slot{};
    };
}
