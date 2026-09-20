#pragma once
#include <cstdint>
#include <lux/engine/meta/MetaAnnotations.hpp>

namespace probe
{
    struct LUX_TYPE_INFO(both)
        LUX_COMM_CONFIG(prefix = ExternalProbe, id = test.external.probe.v1, display = ExternalProbe,
                        feature = UnusedBackend, feature_header = probe / UnusedBackend.hpp) Configuration
    {
    };

    struct LUX_OP(lane = program, kind = bulk, name = ExternalProbeSet, method = set) Value
    {
        std::uint64_t sequence{};
        double amount{};
    };
} // namespace probe
