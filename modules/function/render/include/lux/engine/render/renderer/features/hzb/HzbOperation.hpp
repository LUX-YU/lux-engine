#pragma once
// ============================================================================
//  HzbOperation.hpp — HzbFeature factory (P2 HZB Stage A).
//
//  HzbFeature has no feature-local commands. The factory only creates the
//  feature; use with RenderSession::registerFeatureType(...) + addFeature.
// ============================================================================

#include <lux/engine/function/visibility.h>

namespace lux::render
{
    struct FeatureFactory;

    /// Factory for HzbFeature.
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kHzbFeatureFactory;
}
