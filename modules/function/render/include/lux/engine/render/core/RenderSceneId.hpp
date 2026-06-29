#pragma once
/**
 * @file RenderSceneId.hpp
 * @brief Strong-typed render-scene identifier.
 *
 * Extracted from Renderer.hpp so that game-thread headers
 * can reference the type without pulling in the full Renderer definition.
 */

#include <cstdint>
#include <lux/cxx/container/SlotMap.hpp>   // lux::cxx::SlotKey

namespace lux::render
{

/// Tag for the generational scene handle.
struct RenderSceneTag {};

/// Generational handle into the Renderer's scene slot map (index + generation).
/// A reused scene slot bumps its generation, so a stale id (held after the
/// scene was removed — see Renderer::removeScene async retirement) is rejected
/// by the SlotKeyAutoSparseSet lookup instead of silently aliasing the new
/// scene. Trivially copyable (two uint32) → serializes across the in-process
/// comm channel unchanged in shape, 8 bytes on the wire.
using RenderSceneId = lux::cxx::SlotKey<RenderSceneTag>;

} // namespace lux::render
