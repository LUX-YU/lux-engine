#pragma once
/**
 * @file EntitySceneCookError.hpp
 * @brief Structured failures shared by the generic LXSC/LXES cooker helpers.
 */

#include <lux/engine/toolchain/entity_scene/visibility.h>

#include <cstdint>
#include <string>

namespace lux::toolchain
{
    enum class EEntitySceneCookError : std::uint8_t
    {
        INVALID_ARGUMENT,
        INVALID_NAME_TABLE,
        INVALID_TAGGED_PAYLOAD,
        DUPLICATE_COMPONENT,
        INCONSISTENT_SCHEMA,
        INVALID_ENTITY_REFERENCE,
        INVALID_ATTACHMENT_REFERENCE,
        WIRE_LIMIT_EXCEEDED,
        CONTRACT_REJECTED,
        ENCODE_FAILED
    };

    struct EntitySceneCookFailure final
    {
        EEntitySceneCookError error{
            EEntitySceneCookError::INVALID_ARGUMENT};
        std::string detail;
    };
}
