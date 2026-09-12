param([string]$EvidenceRoot = 'E:/lux-shadow-ui/continuation')
$ErrorActionPreference = 'Stop'
# Keep the qualified candidate launcher and its binary hash checks intact.
# Trace only the small, explicitly selected acceptance alphabet; never arbitrary text.
$previous = $env:LUX_ER1_SELECTED_TEXT_TRACE
try {
    $env:LUX_ER1_SELECTED_TEXT_TRACE = '1'
    & "$EvidenceRoot/launch-desktop.ps1" -Variant diagnostic
    $result = $LASTEXITCODE
} finally {
    $env:LUX_ER1_SELECTED_TEXT_TRACE = $previous
}
exit $result
