#pragma once
/**
 * @file VisualTransitionComponent.hpp
 * @brief Explicit authored policy for visual appearance and retirement fades.
 *
 * Streaming, persistence and stable identity are unrelated facts.  An entity
 * fades only when it carries this component; fixed and spatially streamed
 * content therefore use exactly the same rendering semantics.
 */

#include <lux/engine/meta/MetaAnnotations.hpp>

#include <cstdint>

namespace lux::ecs
{
    struct LUX_COMPONENT() VisualTransitionComponent final
    {
        LUX_MEMBER(display_name=Duration Seconds, min=0.0)
        float duration_seconds{0.35f};

        /// Stable, authored/cooked decorrelation seed.  Zero is normalized to
        /// one by extraction so malformed content cannot disable a fade.
        LUX_MEMBER(display_name=Seed)
        std::uint32_t seed{1u};
    };
}
