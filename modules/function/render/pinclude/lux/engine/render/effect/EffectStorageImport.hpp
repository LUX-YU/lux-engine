#pragma once

#include <lux/engine/render/graph/DrawResource.hpp>
#include <lux/engine/render/core/Resource.hpp>
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

        struct DrawContract final
        {
            cxx::basic_type_info::id_t hash;
            std::string name;
            std::uint32_t required_capabilities;
            bool operator==(const DrawContract&) const noexcept = default;
        };

        std::string binding;
        ResourceName type;
        std::variant<detail::StorageGraphImport, SnapshotContract, DrawContract> contract;
        std::uint32_t projection;
        bool optional = false;
        std::uint32_t graph_uses = 0;
        detail::StorageBindingSlot slot{};
    };
}
