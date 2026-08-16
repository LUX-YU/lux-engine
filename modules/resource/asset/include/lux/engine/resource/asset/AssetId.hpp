#pragma once
/**
 * @file AssetId.hpp
 * @brief Minimal, canonical definition of the engine asset identifier.
 *
 * Asset identifiers cross almost every module boundary. Keeping their alias
 * in Asset.hpp made data-only component and callback headers parse asset
 * serialization, reflection, filesystem and ownership machinery that they do
 * not use. This header is the single lightweight source of that vocabulary.
 */

#include <uuid.h>

namespace lux::asset
{
    using asset_id_t = uuids::uuid;
} // namespace lux::asset
