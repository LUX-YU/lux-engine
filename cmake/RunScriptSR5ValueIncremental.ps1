param(
    [Parameter(Mandatory=$true)][string]$ConsumerRoot,
    [Parameter(Mandatory=$true)][string]$InstalledTemplate,
    [Parameter(Mandatory=$true)][string]$EvidenceRoot
)
$ErrorActionPreference = 'Stop'
. 'D:/Development/Mircosoft/VisualStudio/Common7/Tools/Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
New-Item -ItemType Directory -Force -Path $EvidenceRoot | Out-Null
$build = Join-Path $ConsumerRoot 'build'
$source = Join-Path $ConsumerRoot 'source'
$prefixLine = @(Select-String -LiteralPath "$build/CMakeCache.txt" -Pattern '^CMAKE_PREFIX_PATH:')[0].Line
$prefixes = $prefixLine.Substring($prefixLine.IndexOf('=') + 1).Split(';')
$env:PATH = ((@($prefixes | ForEach-Object { "$_/bin" }) + @('D:/Development/vcpkg/installed/x64-windows/bin', $env:PATH)) -join ';')
$lastRepresentation = $null
$output = @(Get-ChildItem -LiteralPath $build -Recurse -Filter Values.lua.value.generated.hpp)
if ($output.Count -ne 1) { throw 'Expected one actual generated value header' }
$output = $output[0].FullName
$results = [System.Collections.Generic.List[object]]::new()
function Run-Probe([string]$Name, [bool]$Success, [bool]$Change, [bool]$Noop = $false) {
    $before = (Get-FileHash -LiteralPath $output).Hash
    cmake --build $build --target all -j 4 -- -k 0 *> "$EvidenceRoot/$Name.log"
    $exitCode = $LASTEXITCODE
    $after = (Get-FileHash -LiteralPath $output).Hash
    $log = Get-Content -LiteralPath "$EvidenceRoot/$Name.log" -Raw
    $hit = ([regex]::Matches($log, 'lua_values: parse once')).Count
    if (($exitCode -eq 0) -ne $Success) { throw "$Name unexpected exit $exitCode" }
    if ($Change -ne ($before -ne $after)) { throw "$Name output change mismatch" }
    if ($Noop -and $log -notmatch 'no work to do') { throw "$Name not a no-op" }
    if (!$Noop -and $Success -and $hit -ne 1) { throw "$Name expected exactly one value generation, got $hit" }
    if (!$Success -and $log -notmatch 'Lua value|Duplicate Lua|non-public Lua') { throw "$Name wrong negative failure" }
    if ($Success -and $Name -in @('initial-noop','included-field','compile-macro','restored','final-noop')) {
        & "$build/lux_script_lua_values_consumer.exe" *> "$EvidenceRoot/$Name.run.log"
        if ($LASTEXITCODE) { throw "$Name installed execution failed" }
        $business = Get-Content "$EvidenceRoot/$Name.run.log" -Raw
        if ($business -notmatch 'fields=2 id=7 weight=2.5 representation=(\d+) rule=1 PASS') { throw "$Name incomplete business row" }
        $representation = $Matches[1]
        if ($Name -in @('included-field','compile-macro') -and $representation -eq $script:lastRepresentation) {
            throw "$Name did not change the compiled representation"
        }
        $script:lastRepresentation = $representation
    }
    Copy-Item -LiteralPath $output -Destination "$EvidenceRoot/$Name.generated.hpp"
    $results.Add(@{case=$Name;exit=$exitCode;before=$before;after=$after;generation_hits=$hit;expected_success=$Success})
    $results | ConvertTo-Json -Depth 4 | Set-Content "$EvidenceRoot/probes.json"
}
$paths = @('Field.hpp','Values.hpp','Rules.hpp','CMakeLists.txt')
$originals = @{}
foreach ($name in $paths) { $originals[$name] = [IO.File]::ReadAllText((Join-Path $source $name)) }
$templateText = [IO.File]::ReadAllText($InstalledTemplate)
try {
    Run-Probe 'initial-noop' $true $false $true
    [IO.File]::WriteAllText("$source/Field.hpp", $originals['Field.hpp'].Replace('std::int32_t','std::uint32_t'))
    Run-Probe 'included-field' $true $false
    [IO.File]::WriteAllText("$source/CMakeLists.txt", $originals['CMakeLists.txt'] +
        "`ntarget_compile_definitions(lux_script_lua_values_consumer PRIVATE VALUE_SCALAR=std::int32_t)`n")
    Run-Probe 'compile-macro' $true $false
    [IO.File]::WriteAllText("$source/Values.hpp", $originals['Values.hpp'].Replace('name = key','name = number'))
    Run-Probe 'annotation' $true $true
    [IO.File]::WriteAllText("$source/Rules.hpp", $originals['Rules.hpp'].Replace('Revision = 1','Revision = 2'))
    Run-Probe 'custom-rule' $true $false
    [IO.File]::WriteAllText($InstalledTemplate, $templateText + "`n// SR-5 isolated installed-template invalidation probe`n")
    Run-Probe 'installed-template' $true $true
    $wideFields = (0..63 | ForEach-Object { "std::int32_t field$_;" }) -join "`n"
    [IO.File]::WriteAllText("$source/Values.hpp", $originals['Values.hpp'] +
        "`nstruct LUX_META(luxlua::value) WideValue {`n$wideFields`n};`n")
    Run-Probe 'maximum-64-fields' $true $true
    foreach ($negative in @('duplicate','private','capacity','union')) {
        $text = $originals['Values.hpp']
        switch ($negative) {
            duplicate { $text = $text.Replace('double weight;', 'LUX_META(luxlua::field, name = key) double weight;') }
            private { $text = $text.Replace('double weight;', 'private: LUX_META(luxlua::field) double weight;') }
            capacity {
                $fields = (0..64 | ForEach-Object { "std::int32_t field$_;" }) -join "`n"
                $text += "`nstruct LUX_META(luxlua::value) TooMany {`n$fields`n};`n"
            }
            union { $text += "`nunion LUX_META(luxlua::value) InvalidUnion { int x; float y; };`n" }
        }
        [IO.File]::WriteAllText("$source/Values.hpp", $text)
        Run-Probe "reject-$negative" $false $false
    }
} finally {
    foreach ($name in $paths) { [IO.File]::WriteAllText((Join-Path $source $name), $originals[$name]) }
    [IO.File]::WriteAllText($InstalledTemplate, $templateText)
}
Run-Probe 'restored' $true $true
Run-Probe 'final-noop' $true $false $true
