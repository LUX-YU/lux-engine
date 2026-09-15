param(
    [Parameter(Mandatory)][string]$Case,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$Fixture,
    [Parameter(Mandatory)][string]$MaterialFixture,
    [Parameter(Mandatory)][string]$FlowFixture,
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Seed
)
$ErrorActionPreference = 'Stop'
$caseRoot = Join-Path $Root ($Case + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $Root -Force | Out-Null

function Invoke-Checked([string[]]$Arguments, [string]$Marker = '^PASS', [int]$ExpectedExit = 0) {
    $log = $caseRoot + '-' + [guid]::NewGuid().ToString('N') + '.log'
    & $Executable @Arguments *> $log
    $result = $LASTEXITCODE
    Get-Content -LiteralPath $log
    if ($result -ne $ExpectedExit) { throw "Native test exit $result; expected $ExpectedExit. Log: $log" }
    if (-not (Select-String -LiteralPath $log -Pattern $Marker -Quiet)) {
        throw "Native test did not emit its completion marker. Log: $log"
    }
}

if ($Case.StartsWith('publication-')) {
    $mode = $Case.Substring('publication-'.Length)
    if ($mode -eq 'crash') {
        Invoke-Checked @($caseRoot, $mode) '^DELIBERATE INTERRUPTION exit=86' 86
        Invoke-Checked @($caseRoot, 'recover')
    } else {
        Invoke-Checked @($caseRoot, $mode)
    }
    exit 0
}
if ($Case -in @('material_protocol', 'flow_protocol', 'model_protocol', 'import_protocol')) {
    Invoke-Checked @($caseRoot)
    if ($Case -in @('material_protocol', 'flow_protocol')) {
        Invoke-Checked @($caseRoot, 'verify') '^PASS new-process '
    }
    exit 0
}
if ($Case.EndsWith('_protocol') -or $Case -eq 'process_completion') {
    Invoke-Checked @()
    exit 0
}

$fixtureMode = if ($Case -in @('save-hierarchy', 'save-structure')) { 'gpu-hierarchy' }
    elseif ($Case -eq 'save-preservation') { 'gpu-preservation' }
    elseif ($Case -eq 'cpu') { 'cpu' } else { 'gpu' }
& $Fixture $caseRoot $fixtureMode
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item -LiteralPath $Seed -Destination (Join-Path $caseRoot 'Seed.luxpak')
if ($Case -in @('material-gui', 'flow-gui', 'material-publish')) {
    $flow = $Case -eq 'flow-gui'
    $inputRoot = $caseRoot + '-input'
    $inputLog = $caseRoot + '-input.log'
    $inputTool = if ($flow) { $FlowFixture } else { $MaterialFixture }
    & $inputTool $inputRoot *> $inputLog
    $result = $LASTEXITCODE
    Get-Content -LiteralPath $inputLog
    if ($result -ne 0) { exit $result }
    $source = if ($flow) { 'Logic.luxflow' } else { 'Material.luxmaterial' }
    $kind = if ($flow) { 'flow_graph' } else { 'material_graph' }
    Copy-Item -LiteralPath (Join-Path $inputRoot $source) -Destination $caseRoot
    $entry = @"

[[assets]]
id = "00000000-0000-0000-0000-000000000002"
kind = "$kind"
source_path = "$source"
cooked_path = ""
source_digest = ""
compiled_source_digest = ""
mount_path = "Editable"
"@
    Add-Content -LiteralPath (Join-Path $caseRoot 'Project.luxproject') -Value $entry
    if ($Case -eq 'material-gui') {
        Copy-Item -LiteralPath (Join-Path $inputRoot 'Unfinished.luxmaterial') -Destination $caseRoot
        Add-Content -LiteralPath (Join-Path $caseRoot 'Project.luxproject') -Value @"

[[assets]]
id = "00000000-0000-0000-0000-000000000003"
kind = "material_graph"
source_path = "Unfinished.luxmaterial"
cooked_path = ""
source_digest = ""
compiled_source_digest = ""
mount_path = "Editable B"
"@
    }
}
if ($Case -eq 'save-preservation') {
    Copy-Item -LiteralPath (Join-Path $caseRoot 'Main.luxscene') -Destination (Join-Path $caseRoot 'Main.before.luxscene')
}
if ($Case -eq 'save-import-model') {
    Set-Content -LiteralPath (Join-Path $caseRoot 'Triangle.obj') -Encoding utf8 -Value "o Triangle`nv 0 0 0`nv 1 0 0`nv 0 1 0`nf 1 2 3`n"
}
Invoke-Checked @((Join-Path $caseRoot 'Project.luxproject'), $Case) ('^PASS case=' + [regex]::Escape($Case) + ' ')
if ($Case -eq 'save-preservation') {
    & $Fixture $caseRoot verify-preservation
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
exit 0
