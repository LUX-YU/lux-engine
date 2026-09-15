param(
    [Parameter(Mandatory)][string]$Case,
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$Fixture,
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
    exit 0
}
if ($Case.EndsWith('_protocol') -or $Case -eq 'process_completion') {
    Invoke-Checked @()
    exit 0
}

$fixtureMode = if ($Case -in @('save-hierarchy', 'save-structure')) { 'gpu-hierarchy' }
    elseif ($Case -eq 'save-preservation') { 'gpu-preservation' } else { 'gpu' }
& $Fixture $caseRoot $fixtureMode
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item -LiteralPath $Seed -Destination (Join-Path $caseRoot 'Seed.luxpak')
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
