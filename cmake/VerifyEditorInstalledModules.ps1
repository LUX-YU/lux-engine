param(
    [Parameter(Mandatory = $true)][string]$QualificationRoot,
    [Parameter(Mandatory = $true)][string]$CxxPrefix,
    [Parameter(Mandatory = $true)][string]$VcpkgRoot
)
$ErrorActionPreference = 'Stop'
$identity = Get-Content -LiteralPath "$QualificationRoot/qualification.json" -Raw | ConvertFrom-Json
$records = @()
foreach ($location in @('sdk', 'relocated-sdk')) {
    $prefix = (Resolve-Path -LiteralPath "$QualificationRoot/$location").Path
    $bin = Join-Path $prefix 'bin'
    $engineNames = @(Get-ChildItem -LiteralPath $bin -Filter '*.dll' | ForEach-Object Name)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = "$QualificationRoot/consumers/$location/editor-tooling/editor_tooling_consumer.exe"
    $start.Arguments = '--hold-for-module-audit'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $cleanPath = ($env:PATH -split ';' | Where-Object {
        $_ -and $_ -notmatch '(?i)lux-er1|lux-sv1|install[/\\]RelWithDebInfo|build[/\\]RelWithDebInfo'
    }) -join ';'
    $start.Environment['PATH'] = "$bin;$CxxPrefix/bin;$VcpkgRoot/installed/x64-windows/bin;$cleanPath"
    $process = [Diagnostics.Process]::Start($start)
    try {
        $lineTask = $process.StandardOutput.ReadLineAsync()
        if (!$lineTask.Wait(10000) -or $lineTask.Result -notmatch 'installed Toolset PASS') {
            throw "Consumer did not reach the actual completed protocol: $location"
        }
        $process.Refresh()
        $modules = @($process.Modules | ForEach-Object {
            $path = $_.FileName
            if ($_.ModuleName -in $engineNames -and
                !([IO.Path]::GetDirectoryName($path)).Equals($bin, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Installed consumer loaded an engine DLL from the wrong prefix: $path"
            }
            [pscustomobject]@{ name = $_.ModuleName; path = $path;
                sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
        })
        if (!($modules | Where-Object name -eq 'lux_engine_editor_tooling.dll')) {
            throw 'Actual Toolset DLL absent from loaded modules'
        }
        $process.StandardInput.WriteLine()
        $process.StandardInput.Close()
        if (!$process.WaitForExit(10000) -or $process.ExitCode -ne 0) { throw 'Consumer exit failed' }
        $records += [pscustomobject]@{ location = $location; source_commit = $identity.source_commit;
            prefix = $prefix; protocol = $lineTask.Result; exit = $process.ExitCode; modules = $modules }
    } finally {
        if (!$process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}
$records | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath "$QualificationRoot/installed-loaded-modules.json" -Encoding utf8
Write-Output 'Both installed Toolset consumers loaded the actual DLL exclusively from their selected SDK prefix'
