#pragma once

#if !defined(LUX_OBJECT_TEST_DIAGNOSTICS)
#error Object diagnostics are available only to the benchmark test build
#endif

#include <cstdint>

#include <lux/engine/object/LuxObject.hpp>

namespace lux::object::detail
{
    struct ObjectDiagnosticsAccess final
    {
        [[nodiscard]] static std::uint64_t storageGrowthCount(
            const LuxObject& object
        ) noexcept
        {
            return object.storageGrowthCountForTest();
        }
    };
} // namespace lux::object::detail
