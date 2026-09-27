#pragma once
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/editor/EditorError.hpp>
#include <exception>

namespace lux::editor::detail
{
    inline object::Connection takeConnection(object::LuxObject::ConnectResult connected, EditorResult<void>& status)
    {
        if (connected)
            return std::move(*connected);
        if (connected.error() == object::EConnectError::ALLOCATION_FAILURE)
            std::terminate();
        const bool is_capacity_exhausted = connected.error() == object::EConnectError::CAPACITY_EXHAUSTED;
        if (status)
            status = lux::cxx::unexpected(EditorFailure{
                is_capacity_exhausted ? EEditorError::CAPACITY : EEditorError::FRONTEND_FAILURE,
                "object.connect",
                static_cast<std::uint64_t>(connected.error())
            });
        return {};
    }

    inline void reportSignalDelivery(object::SignalDelivery result, const char* event) noexcept
    {
        if (!result.complete())
            log::warn(
                "editor.signal",
                "{}: {} recipients full, {} closed; owner state retained",
                event,
                result.full,
                result.closed
            );
    }
}
