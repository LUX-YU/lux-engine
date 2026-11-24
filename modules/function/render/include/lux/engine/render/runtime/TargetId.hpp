#pragma once

#include <lux/cxx/container/SlotMap.hpp>

namespace lux::render
{
    enum class EProductRead : std::uint8_t { UNSPECIFIED, CURRENT_RUN, LATEST_PUBLISHED };

    struct TargetTag;
    using TargetId = cxx::SlotKey<TargetTag>;
} // namespace lux::render
