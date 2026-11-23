#pragma once

#include <lux/cxx/container/SlotMap.hpp>

namespace lux::render
{
    struct DeviceSessionTag;
    using DeviceSessionId = cxx::SlotKey<DeviceSessionTag>;
} // namespace lux::render
