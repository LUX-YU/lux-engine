#pragma once
/**
 * @file Slot.hpp
 * @brief SlotHandle — untyped generational handle, now an alias for
 *        lux::cxx::SlotKey<void>.
 *
 * @note For *new* handle types prefer TypedSlotHandle<Tag> (ResourceHandle.hpp)
 *       which adds compile-time tag safety.
 */
#include <lux/cxx/container/SlotMap.hpp>

namespace lux::render
{
    using SlotHandle = lux::cxx::SlotKey<>;
}
