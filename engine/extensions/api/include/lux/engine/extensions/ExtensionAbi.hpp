#pragma once
/**
 * @file ExtensionAbi.hpp
 * @brief Stable public include path for the Lux Engine Extension ABI.
 *
 * This is intentionally a compatibility forwarding header while the lower
 * layers are being disentangled from the historical core/extension_abi
 * component. Keeping exactly one definition of the ABI types prevents a
 * translation unit from seeing duplicate declarations through mixed old and
 * new include paths. The definitions move here only after Resource and ECS no
 * longer depend on the legacy component.
 */

#include <lux/engine/core/extension_abi/ModuleAbi.hpp>
