#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/resource/Storage.hpp>
#include <string>
#include <vector>

namespace lux::render
{
    // Cold placement lists. No scope identity, parent tree or runtime authority is allocated here.
    template <class T> struct StorageGroups final
    {
        std::vector<T> global;
        std::vector<T> scene;
        std::vector<T> view;
        std::vector<T> attachment;

        [[nodiscard]] std::size_t size() const noexcept
        {
            return global.size() + scene.size() + view.size() + attachment.size();
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return size() == 0;
        }
    };

    struct GraphStorageBinding final
    {
        GraphResourceId resource;
        StorageRequirement storage;
    };

    using GraphStorageBindings = StorageGroups<GraphStorageBinding>;

    // Effect-owned concrete arguments, resolved only at Attach; never part of immutable Native code.
    struct StorageArgument final
    {
        std::string resource_key;
        ConfigValue value;
    };

} // namespace lux::render
