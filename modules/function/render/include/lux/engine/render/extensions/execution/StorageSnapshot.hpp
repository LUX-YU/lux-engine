#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <cstdint>
#include <functional>

namespace lux::render
{
    enum class ESnapshotScope : std::uint8_t
    {
        ANY_ON_DEVICE,
        JOB_LOCAL
    };

    namespace extension_detail
    {
        struct SnapshotOperand final
        {
            std::uint32_t frame, program;
            const cxx::basic_type_info* type;
        };
    }

    template <class T> class StorageSnapshotSlot final
    {
        friend class CpuCallFrame;
        friend class RasterProgramBuildContext;
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
