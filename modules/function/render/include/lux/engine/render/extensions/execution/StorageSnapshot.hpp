#pragma once

#include <cstdint>
#include <functional>

namespace lux::render
{
    template <class T> class StorageSnapshotSlot final
    {
        friend class FramePreparation;
        friend class RasterProgramSelection;
        friend class InstanceLinkContext;
        explicit constexpr StorageSnapshotSlot(std::uint32_t slot) noexcept : slot_(slot) {}
        std::uint32_t slot_;
    };

    template <class T> struct PreparedStorageSnapshot final
    {
        std::reference_wrapper<const T> value;
        std::uint64_t revision;
    };

    namespace extension_detail
    {
        struct SelectedStorageSnapshot final
        {
            const void* value;
            std::uint64_t revision;
        };
    }
} // namespace lux::render
