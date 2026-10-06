param([Parameter(Mandatory=$true)][string]$Prefix)
$ErrorActionPreference = 'Stop'
$executable = Join-Path $Prefix 'bin/lux_editor.exe'
$process = Start-Process -FilePath $executable -WorkingDirectory $Prefix -WindowStyle Hidden -PassThru
$deadline = [DateTime]::UtcNow.AddSeconds(20)
do {
    Start-Sleep -Milliseconds 100
    $process.Refresh()
    if ($process.HasExited) { throw "Installed product exited before creating a window: $($process.ExitCode)" }
} while ($process.MainWindowHandle -eq 0 -and [DateTime]::UtcNow -lt $deadline)
if ($process.MainWindowHandle -eq 0) { throw 'Installed product window not created' }
Start-Sleep -Seconds 2
$process.Refresh()
Write-Output "Installed product PID=$($process.Id), title=$($process.MainWindowTitle)"
foreach ($module in $process.Modules) {
    $path = $module.FileName.Replace('\','/')
    Write-Output $path
    if ($path -match '_legacy|/CodeRepos/build/|/lux-engine-ec2/') { throw "Unexpected module: $path" }
    if ($path -match '/CodeRepos/install/' -and -not $path.StartsWith($Prefix.Replace('\','/'), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Old SDK module fallback: $path"
    }
}
if (-not $process.CloseMainWindow()) { throw 'Close message was refused' }
if (-not $process.WaitForExit(20000)) { throw 'Installed product failed to retire after close' }
if ($process.ExitCode -ne 0) { throw "Installed product exit: $($process.ExitCode)" }
Write-Output 'PASS actual installed lux_editor: native window, isolated loaded DLL closure, WM_CLOSE and retirement'
