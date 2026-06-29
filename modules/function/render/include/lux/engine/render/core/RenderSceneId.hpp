#pragma once
/**
 * @file RenderSceneId.hpp
 * @brief Strong-typed render-scene identifier.
 *
 * Extracted from Renderer.hpp so that game-thread headers
 * can reference the type without pulling in the full Renderer definition.
 */

#include <cstdint>

namespace lux::render
{

/// Strongly-typed index into the Renderer's scene sparse set.
enum class RenderSceneId : uint32_t {};

} // namespace lux::render
