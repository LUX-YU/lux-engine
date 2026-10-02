#pragma once
namespace lux::editor::desktop::testing
{
    // Test-only fault at the real mandatory connection admission; never installed in the SDK.
    void rejectNextMenuConnection() noexcept;
}
