#pragma once

#include <lux/engine/platform/ipc/IpcMessage.hpp>

// Project-only trusted boundary, not part of the installed IPC API. Runtime has already established the
// canonical type from its immutable command entry and a typed invocation before using these functions.
namespace lux::platform::ipc::detail
{
    // T must be nothrow-movable. Constructs by moving the live T named by canonical; source remains a valid T.
    LUX_PLATFORM_IPC_PUBLIC void moveFrom(IpcMessage& message, const IpcTypeOps& canonical, void* object,
        std::shared_ptr<const void> code_pin = {}) noexcept;

    // Requires a nonempty message with its canonical type checked at the receiving boundary.
    [[nodiscard]] LUX_PLATFORM_IPC_PUBLIC void* borrowObject(IpcMessage& message) noexcept;
    [[nodiscard]] LUX_PLATFORM_IPC_PUBLIC const void* borrowObject(const IpcMessage& message) noexcept;
}
