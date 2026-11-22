#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <cstdint>
#include <functional>

namespace lux::render
{
    struct GraphStorageSnapshotId final
    {
        std::uint32_t index = kGraphInvalidIndex;
        bool operator==(const GraphStorageSnapshotId&) const noexcept = default;
    };

    enum class ESnapshotScope : std::uint8_t
    {
        ANY_ON_DEVICE,
        JOB_LOCAL
    };

    namespace extension_detail
    {
        struct SnapshotOperand final
        {
            std::uint32_t frame;
            const cxx::basic_type_info* type;
        };
    }

    template <class T> class StorageSnapshotSlot final
    {
        friend class CpuCallFrame;
        friend class InstanceLinkContext;
        explicit constexpr StorageSnapshotSlot(extension_detail::SnapshotOperand operand) noexcept
            : operand_(operand) {}
        extension_detail::SnapshotOperand operand_;
    };

    template <class T> struct RevisionedValue final
    {
        T value;
        std::uint64_t revision;
    };
} // namespace lux::render
