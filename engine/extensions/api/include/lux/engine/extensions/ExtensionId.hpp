#pragma once
/**
 * @file ExtensionId.hpp
 * @brief Narrow Engine-owned include path for extension identity.
 *
 * The underlying definition remains in the frozen compatibility component
 * until Resource and ECS consumers no longer include the historical ABI
 * header. Engine code should include this header instead of reaching into
 * modules/core/extension_abi directly.
 */

#include <lux/engine/core/extension_abi/StableId.hpp>
