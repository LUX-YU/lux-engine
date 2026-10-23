#pragma once

#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace lux::test
{
    // Unattended tests must report failures to their captured stream, never a modal CRT dialog.
    // Preserve assertion checks and use fatal process termination, including during DLL teardown.
    inline void configureProcessDiagnostics() noexcept
    {
#ifdef _MSC_VER
        _set_error_mode(_OUT_TO_STDERR);
        // Keep the fatal-report path used by Release, including abort during DLL teardown.
        // Disabling that path can leave an abort during DLL teardown with the original exit code 0.
        _set_abort_behavior(_CALL_REPORTFAULT, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    }
} // namespace lux::test
